// The root view of a sandboxed game: a fresh tmpfs, read-only binds, private /tmp and /dev/shm, a filtered runtime
// folder, the per-session overlay and the game's one writable session folder; then pivot_root and the environment.
// See review-F1.md 4.2 and 5.3-5.4. Every failure is fail-closed.

#include "hf-sandbox.h"

#include <linux/mount.h>

static int target_path(const struct hf_sandbox *sandbox, const char *absolute, char *out, size_t size)
{
    if (absolute[0] != '/') return hf_fail_msg("an internal path is not absolute: %s", absolute);
    if (absolute[1] == '\0') return hf_join(out, size, sandbox->root, ".");
    return hf_join(out, size, sandbox->root, absolute + 1);
}

static bool path_covers(const char *ancestor, const char *path)
{
    size_t length = strlen(ancestor);
    while (length > 1 && ancestor[length - 1] == '/') length--;
    if (length == 1 && ancestor[0] == '/') return path[0] == '/';
    if (length == 0 || strncmp(ancestor, path, length) != 0) return false;
    return path[length] == '\0' || path[length] == '/';
}

// The optional system folder can itself be a symlink (Fedora Atomic's /opt -> /var/opt); its bind points at the name,
// but the content is the resolved folder. Skip it if either name covers the real installation or prefix.
static bool optional_covers(const char *system, const char *path)
{
    if (path_covers(system, path)) return true;
    char resolved[PATH_MAX];
    if (realpath(system, resolved) == NULL) return false;
    return path_covers(resolved, path);
}

static int ensure_target(const char *target, bool directory)
{
    if (make_parent_dirs(target) != 0) return hf_fail("creating a mount target");
    if (directory)
    {
        struct stat info;
        if (mkdir(target, 0755) != 0 && errno != EEXIST) return hf_fail("creating a mount target folder");
        if (stat(target, &info) != 0 || !S_ISDIR(info.st_mode))
            return hf_fail_msg("a mount target is not a folder: %s", target);
        return 0;
    }
    int fd = open(target, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return hf_fail("creating a mount target file");
    close(fd);
    return 0;
}

static int set_readonly(const char *target, bool recursive)
{
    struct mount_attr attributes;
    memset(&attributes, 0, sizeof(attributes));
    attributes.attr_set = MOUNT_ATTR_RDONLY;
    if (syscall(SYS_mount_setattr, AT_FDCWD, target, recursive ? AT_RECURSIVE : 0, &attributes, sizeof(attributes)) != 0)
        return hf_fail("making a bind mount read-only");
    return 0;
}

static int bind_readonly(struct hf_sandbox *sandbox, const char *source, const char *target_absolute)
{
    char target[PATH_MAX];
    struct stat info;
    if (stat(source, &info) != 0) return hf_fail(source);
    if (target_path(sandbox, target_absolute, target, sizeof(target)) != 0) return 1;
    if (ensure_target(target, S_ISDIR(info.st_mode)) != 0) return 1;
    if (mount(source, target, NULL, MS_BIND | (S_ISDIR(info.st_mode) ? MS_REC : 0), NULL) != 0)
        return hf_fail("bind-mounting a read-only path");
    if (set_readonly(target, S_ISDIR(info.st_mode)) != 0) return 1;
    return hf_add_rule(sandbox, target_absolute, S_ISDIR(info.st_mode) ? HF_ACCESS_RO : HF_ACCESS_FILE_RO);
}

static int bind_writable_dir(struct hf_sandbox *sandbox, const char *source, const char *target_absolute)
{
    char target[PATH_MAX];
    struct stat info;
    if (stat(source, &info) != 0 || !S_ISDIR(info.st_mode))
        return hf_fail_msg("a writable sandbox path is not a folder: %s", source);
    if (target_path(sandbox, target_absolute, target, sizeof(target)) != 0) return 1;
    if (ensure_target(target, true) != 0) return 1;
    if (mount(source, target, NULL, MS_BIND | MS_REC, NULL) != 0) return hf_fail("bind-mounting a writable path");
    return hf_add_rule(sandbox, target_absolute, HF_ACCESS_RW);
}

// The default installation lives inside the prefix the overlay lower is made from. Its lower-layer copy would make the
// installation (profiles, mods, user data, account snapshot) visible through C:. Mount an empty read-only tmpfs over
// that one path inside merged and put only the game copy back, as a non-recursive read-only bind. gameOut is not bound
// here: merged is under gameOut, so that would be a loop; Z: already exposes gameOut.
int hf_mask_overlay_install(struct hf_sandbox *sandbox)
{
    if (!sandbox->install_in_prefix) return 0;
    if (mount("tmpfs", sandbox->overlay_install, "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755,size=64k") != 0)
        return hf_fail("mounting the overlay installation mask");
    char game[PATH_MAX];
    if (hf_join(game, sizeof(game), sandbox->overlay_install, "game") != 0) return hf_fail("the masked game path");
    if (make_dir(game) != 0) return hf_fail("creating the masked game path");
    if (mount(sandbox->game, game, NULL, MS_BIND, NULL) != 0) return hf_fail("bind-mounting the masked game copy");
    if (set_readonly(game, false) != 0) return 1;
    if (set_readonly(sandbox->overlay_install, true) != 0) return 1;
    if (hf_add_rule(sandbox, sandbox->overlay_install, HF_ACCESS_RO) != 0) return 1;
    hf_note("masking the installation in the overlay at %s", sandbox->overlay_install);
    return 0;
}

// One existing regular file made writable over a read-only directory bind. Used only for Proton's dist.lock: the target
// already exists in the read-only Proton bind, so this must not create it (that would write the real Proton folder).
static int bind_proton_lock(struct hf_sandbox *sandbox)
{
    if (sandbox->proton_lock[0] == '\0') return 0;
    struct stat info;
    if (lstat(sandbox->proton_lock, &info) != 0 || !S_ISREG(info.st_mode))
        return hf_fail_msg("Proton's dist.lock is not a regular file: %s", sandbox->proton_lock);
    char target[PATH_MAX];
    if (target_path(sandbox, sandbox->proton_lock, target, sizeof(target)) != 0) return 1;
    if (mount(sandbox->proton_lock, target, NULL, MS_BIND, NULL) != 0)
        return hf_fail("bind-mounting Proton's dist.lock");
    return hf_add_rule(sandbox, sandbox->proton_lock, HF_ACCESS_PROTON_LOCK);
}

static int bind_device_file(struct hf_sandbox *sandbox, const char *source, const char *target_absolute)
{
    char target[PATH_MAX];
    if (target_path(sandbox, target_absolute, target, sizeof(target)) != 0) return 1;
    if (ensure_target(target, false) != 0) return 1;
    if (mount(source, target, NULL, MS_BIND, NULL) != 0) return hf_fail("bind-mounting a device node");
    return 0;
}

static int mount_tmpfs(struct hf_sandbox *sandbox, const char *target_absolute, const char *options, uint64_t access)
{
    char target[PATH_MAX];
    if (target_path(sandbox, target_absolute, target, sizeof(target)) != 0) return 1;
    if (ensure_target(target, true) != 0) return 1;
    if (mount("tmpfs", target, "tmpfs", MS_NOSUID | MS_NODEV, options) != 0) return hf_fail("mounting a private tmpfs");
    return hf_add_rule(sandbox, target_absolute, access);
}

static int copy_symlink(struct hf_sandbox *sandbox, const char *absolute)
{
    char source[PATH_MAX], target[PATH_MAX], link[PATH_MAX];
    if (snprintf(source, sizeof(source), "%s", absolute) >= (int)sizeof(source)) return hf_fail_msg("a system link path is too long");
    ssize_t length = readlink(source, link, sizeof(link) - 1);
    if (length < 0) return hf_fail("reading a system link");
    link[length] = '\0';
    if (target_path(sandbox, source, target, sizeof(target)) != 0) return 1;
    if (make_parent_dirs(target) != 0) return hf_fail("creating a system link's parent");
    if (symlink(link, target) != 0) return hf_fail("recreating a system link");
    return 0;
}

static int bind_required_system_dirs(struct hf_sandbox *sandbox)
{
    static const char *const required[] = { "/usr", "/etc" };
    for (size_t index = 0; index < sizeof(required) / sizeof(required[0]); index++)
    {
        struct stat info;
        if (stat(required[index], &info) != 0 || !S_ISDIR(info.st_mode))
            return hf_fail_msg("a required system folder is missing: %s", required[index]);
        if (bind_readonly(sandbox, required[index], required[index]) != 0) return 1;
    }
    return 0;
}

static int bind_optional_system_dirs(struct hf_sandbox *sandbox)
{
    // /var is not bound whole: Fedora Atomic's real home is /var/home, and an installation there must not become visible
    // through the system bind (profiles/mods/userdata would stop being absent). Wine/NSS/fontconfig need only these
    // subtrees under /var. An optional folder that covers the installation is skipped, not mounted read-only over it.
    static const char *const optional[] = { "/opt", "/var/lib", "/var/cache" }; // /usr/local is under /usr
    for (size_t index = 0; index < sizeof(optional) / sizeof(optional[0]); index++)
    {
        struct stat info;
        if (stat(optional[index], &info) != 0 || !S_ISDIR(info.st_mode)) continue;
        if (optional_covers(optional[index], sandbox->install) || optional_covers(optional[index], sandbox->prefix) ||
            optional_covers(optional[index], sandbox->host_home)) continue;
        if (bind_readonly(sandbox, optional[index], optional[index]) != 0) return 1;
    }
    return 0;
}

static int bind_system_links(struct hf_sandbox *sandbox)
{
    static const char *const top_links[] = { "/bin", "/sbin", "/lib", "/lib64", "/lib32", "/libx32" };
    for (size_t index = 0; index < sizeof(top_links) / sizeof(top_links[0]); index++)
    {
        struct stat info;
        if (lstat(top_links[index], &info) != 0) continue;
        if (S_ISLNK(info.st_mode))
        {
            if (copy_symlink(sandbox, top_links[index]) != 0) return 1;
        }
        else if (S_ISDIR(info.st_mode) && bind_readonly(sandbox, top_links[index], top_links[index]) != 0) return 1;
    }
    return 0;
}

static int mount_system_dirs(struct hf_sandbox *sandbox)
{
    if (bind_required_system_dirs(sandbox) != 0) return 1;
    if (bind_optional_system_dirs(sandbox) != 0) return 1;
    if (bind_system_links(sandbox) != 0) return 1;
    struct stat info;
    if (lstat("/var/run", &info) == 0 && S_ISLNK(info.st_mode) && copy_symlink(sandbox, "/var/run") != 0) return 1;
    return 0;
}

static int mount_proc_and_sys(struct hf_sandbox *sandbox)
{
    struct stat info;
    // Phase 2 keeps the host's PID view, so /proc is the host's, read-only. Phase 3 replaces it with a private /proc in
    // the game's PID namespace; the seam is hf_enter_namespaces.
    if (stat("/proc", &info) != 0 || !S_ISDIR(info.st_mode)) return hf_fail_msg("the host has no /proc");
    if (bind_readonly(sandbox, "/proc", "/proc") != 0) return 1;
    if (stat("/sys", &info) == 0 && S_ISDIR(info.st_mode) && bind_readonly(sandbox, "/sys", "/sys") != 0) return 1;
    return 0;
}

static int bind_device_name(struct hf_sandbox *sandbox, const char *name, bool required)
{
    char source[PATH_MAX], target[PATH_MAX];
    struct stat info;
    if (hf_join(source, sizeof(source), "/dev", name) != 0) return hf_fail("a device path");
    if (lstat(source, &info) != 0 || !S_ISCHR(info.st_mode))
        return required ? hf_fail_msg("a required device node is missing: %s", source) : 0;
    if (snprintf(target, sizeof(target), "/dev/%s", name) >= (int)sizeof(target)) return hf_fail_msg("a device path is too long");
    return bind_device_file(sandbox, source, target);
}

static int mount_dev_nodes(struct hf_sandbox *sandbox)
{
    static const char *const required[] = { "null", "zero", "full", "random", "urandom" };
    static const char *const optional[] = { "tty", "nvidiactl", "nvidia0", "nvidia-modeset", "nvidia-uvm", "nvidia-uvm-tools" };
    for (size_t index = 0; index < sizeof(required) / sizeof(required[0]); index++)
        if (bind_device_name(sandbox, required[index], true) != 0) return 1;
    for (size_t index = 0; index < sizeof(optional) / sizeof(optional[0]); index++)
        if (bind_device_name(sandbox, optional[index], false) != 0) return 1;
    return 0;
}

static int mount_dev_dirs(struct hf_sandbox *sandbox)
{
    static const char *const dirs[] = { "/dev/dri", "/dev/nvidia-caps" };
    for (size_t index = 0; index < sizeof(dirs) / sizeof(dirs[0]); index++)
    {
        struct stat info;
        if (stat(dirs[index], &info) != 0 || !S_ISDIR(info.st_mode)) continue;
        char target[PATH_MAX];
        if (target_path(sandbox, dirs[index], target, sizeof(target)) != 0) return 1;
        if (ensure_target(target, true) != 0) return 1;
        if (mount(dirs[index], target, NULL, MS_BIND | MS_REC, NULL) != 0) return hf_fail("bind-mounting a device folder");
        if (hf_add_rule(sandbox, dirs[index], HF_ACCESS_DEVICE | LANDLOCK_ACCESS_FS_READ_DIR) != 0) return 1;
    }
    return 0;
}

static int mount_dev_links(struct hf_sandbox *sandbox)
{
    // Some programs open these names; they only resolve inside the private /proc. Best effort: their absence is normal.
    const char *links[][2] = { { "/dev/fd", "/proc/self/fd" }, { "/dev/stdin", "/proc/self/fd/0" },
        { "/dev/stdout", "/proc/self/fd/1" }, { "/dev/stderr", "/proc/self/fd/2" } };
    for (size_t index = 0; index < sizeof(links) / sizeof(links[0]); index++)
    {
        char target[PATH_MAX];
        if (target_path(sandbox, links[index][0], target, sizeof(target)) != 0) return 1;
        symlink(links[index][1], target);
    }
    return 0;
}

static int mount_dev(struct hf_sandbox *sandbox)
{
    char dev[PATH_MAX];
    if (target_path(sandbox, "/dev", dev, sizeof(dev)) != 0) return 1;
    if (ensure_target(dev, true) != 0) return 1;
    // /dev is a tmpfs without MS_NODEV: the only device nodes in it are the ones bound below, and a nodev tmpfs can make
    // a bind-mounted device node unusable on some kernels. The forbidden nodes are never created or bound.
    if (mount("tmpfs", dev, "tmpfs", MS_NOSUID, "mode=0755,size=16m") != 0) return hf_fail("mounting the private /dev");
    if (hf_add_rule(sandbox, "/dev", HF_ACCESS_DEVICE | LANDLOCK_ACCESS_FS_READ_DIR) != 0) return 1;
    if (mount_dev_nodes(sandbox) != 0) return 1;
    if (mount_dev_dirs(sandbox) != 0) return 1;
    if (mount_tmpfs(sandbox, "/dev/shm", "mode=1777,size=2g", HF_ACCESS_RW) != 0) return 1;
    if (mount_dev_links(sandbox) != 0) return 1;
    if (chmod(dev, 0555) != 0) return hf_fail("restricting /dev to its nodes");
    return 0;
}

static int bind_runtime_socket(struct hf_sandbox *sandbox, const char *source, const char *target_absolute)
{
    struct stat info;
    if (lstat(source, &info) != 0) return hf_fail(source);
    if (!S_ISSOCK(info.st_mode)) return hf_fail_msg("a runtime bind source is not a socket: %s", source);
    char target[PATH_MAX];
    if (target_path(sandbox, target_absolute, target, sizeof(target)) != 0) return 1;
    if (ensure_target(target, false) != 0) return 1;
    if (mount(source, target, NULL, MS_BIND, NULL) != 0)
    {
        int saved = errno;
        return hf_fail_msg("bind-mounting a runtime socket: %s at %s: %s (errno %d)",
            source, target_absolute, strerror(saved), saved);
    }
    // A socket inode cannot be opened for writing; the runtime folder's Landlock rule covers it.
    return 0;
}

// Xauthority and the PulseAudio cookie can be unlinked while still open: pressure-vessel bind-mounts a temporary file
// and deletes it, leaving a path with link count 0 that the kernel refuses to bind-mount again. These small credential
// files are copied into the private runtime folder from an fd opened with O_NOFOLLOW instead.
static int open_credential(const char *source)
{
    int input = open(source, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (input < 0)
    {
        hf_note("%s could not be opened (%s); not exposing it", source, strerror(errno));
        return -1;
    }
    struct stat info;
    if (fstat(input, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        info.st_size > HF_MAX_CREDENTIAL_BYTES)
    {
        hf_note("%s is not a regular file of at most %d bytes; not exposing it", source, HF_MAX_CREDENTIAL_BYTES);
        close(input);
        return -1;
    }
    return input;
}

// 0: copied; 1: read/write error; 2: the source grew beyond the cap.
static int copy_credential_bytes(int input, int output)
{
    char buffer[4096];
    size_t total = 0;
    ssize_t got;
    while ((got = read(input, buffer, sizeof(buffer))) > 0)
    {
        if (total + (size_t)got > HF_MAX_CREDENTIAL_BYTES) return 2;
        if (write_all(output, buffer, (size_t)got) != 0) return 1;
        total += (size_t)got;
    }
    return got < 0 ? 1 : 0;
}

static int copy_runtime_file(struct hf_sandbox *sandbox, const char *source, const char *target_absolute,
    uint64_t access, bool *copied)
{
    *copied = false;
    int input = open_credential(source);
    if (input < 0) return 0;
    char target[PATH_MAX];
    if (target_path(sandbox, target_absolute, target, sizeof(target)) != 0)
    {
        close(input);
        return 1;
    }
    if (make_parent_dirs(target) != 0)
    {
        close(input);
        return hf_fail("creating a credential target folder");
    }
    int output = open(target, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0400);
    if (output < 0)
    {
        close(input);
        return hf_fail("creating a credential file");
    }
    int result = copy_credential_bytes(input, output);
    int saved = errno;
    close(input);
    if (result == 2)
    {
        close(output);
        unlink(target);
        hf_note("%s grew beyond %d bytes; not exposing it", source, HF_MAX_CREDENTIAL_BYTES);
        return 0;
    }
    if (result != 0)
    {
        close(output);
        errno = saved;
        return hf_fail("copying a credential file");
    }
    if (fchmod(output, 0400) != 0) return hf_fail("restricting a credential file");
    if (close(output) != 0) return hf_fail("finishing a credential file");
    if (hf_add_rule(sandbox, target_absolute, access) != 0) return 1;
    *copied = true;
    return 0;
}

static int bind_wayland(struct hf_sandbox *sandbox)
{
    if (sandbox->wayland_socket[0] == '\0') return 0;
    // Bind at the name WAYLAND_DISPLAY names, not at the resolved target's name: a link may point anywhere inside
    // the runtime folders, while the game opens the name it was given.
    const char *name = sandbox->wayland_name[0] == '\0' ? "wayland-0" : sandbox->wayland_name;
    char target[PATH_MAX];
    if (snprintf(target, sizeof(target), "%s/%s", sandbox->runtime, name) >= (int)sizeof(target))
        return hf_fail_msg("the Wayland socket path is too long");
    if (bind_runtime_socket(sandbox, sandbox->wayland_socket, target) != 0) return 1;
    if (setenv("WAYLAND_DISPLAY", name, 1) != 0) return hf_fail("setting WAYLAND_DISPLAY");
    return 0;
}

static int bind_pulse_socket(struct hf_sandbox *sandbox)
{
    if (sandbox->pulse_socket[0] == '\0') return 0;
    char target[PATH_MAX];
    if (snprintf(target, sizeof(target), "%s/pulse/native", sandbox->runtime) >= (int)sizeof(target))
        return hf_fail_msg("the PulseAudio socket path is too long");
    if (bind_runtime_socket(sandbox, sandbox->pulse_socket, target) != 0) return 1;
    if (snprintf(target, sizeof(target), "unix:%s/pulse/native", sandbox->runtime) >= (int)sizeof(target))
        return hf_fail_msg("the PulseAudio server path is too long");
    if (setenv("PULSE_SERVER", target, 1) != 0) return hf_fail("setting PULSE_SERVER");
    return 0;
}

static int copy_pulse_cookie(struct hf_sandbox *sandbox)
{
    if (sandbox->pulse_cookie[0] == '\0') return 0;
    char target[PATH_MAX];
    if (snprintf(target, sizeof(target), "%s/pulse/cookie", sandbox->runtime) >= (int)sizeof(target))
        return hf_fail_msg("the PulseAudio cookie path is too long");
    bool copied;
    if (copy_runtime_file(sandbox, sandbox->pulse_cookie, target, HF_ACCESS_FILE_RO, &copied) != 0) return 1;
    if (!copied) return 0;
    if (setenv("PULSE_COOKIE", target, 1) != 0) return hf_fail("setting PULSE_COOKIE");
    return 0;
}

static int bind_pulse(struct hf_sandbox *sandbox)
{
    if (bind_pulse_socket(sandbox) != 0) return 1;
    return copy_pulse_cookie(sandbox);
}

static int bind_x11(struct hf_sandbox *sandbox)
{
    if (sandbox->x11_socket[0] == '\0') return 0;
    const char *name = sandbox->x11_name[0] == '\0' ? "X0" : sandbox->x11_name;
    char target[PATH_MAX];
    if (snprintf(target, sizeof(target), "/tmp/.X11-unix/%s", name) >= (int)sizeof(target))
        return hf_fail_msg("the X11 socket path is too long");
    if (bind_runtime_socket(sandbox, sandbox->x11_socket, target) != 0) return 1;
    return 0;
}

static int create_runtime_dir(struct hf_sandbox *sandbox)
{
    char runtime_target[PATH_MAX];
    if (target_path(sandbox, sandbox->runtime, runtime_target, sizeof(runtime_target)) != 0) return 1;
    // Do not hardcode /run/user: the runtime path is the host's when XDG_RUNTIME_DIR was set (a nested user namespace
    // reports uid 0 even though the host runtime is /run/user/1000).
    if (make_parent_dirs(runtime_target) != 0 || make_dir(runtime_target) != 0)
        return hf_fail("creating the private runtime folder");
    if (chmod(runtime_target, 0700) != 0) return hf_fail("restricting the private runtime folder");
    return hf_add_rule(sandbox, sandbox->runtime, HF_ACCESS_RW);
}

static int bind_pipewire(struct hf_sandbox *sandbox)
{
    if (sandbox->pipewire_socket[0] == '\0') return 0;
    char target[PATH_MAX];
    if (snprintf(target, sizeof(target), "%s/pipewire-0", sandbox->runtime) >= (int)sizeof(target))
        return hf_fail_msg("the PipeWire socket path is too long");
    if (bind_runtime_socket(sandbox, sandbox->pipewire_socket, target) != 0) return 1;
    if (setenv("PIPEWIRE_REMOTE", "pipewire-0", 1) != 0) return hf_fail("setting PIPEWIRE_REMOTE");
    return 0;
}

static int copy_xauthority(struct hf_sandbox *sandbox)
{
    if (sandbox->xauthority[0] == '\0') return 0;
    char target[PATH_MAX];
    if (snprintf(target, sizeof(target), "%s/.Xauthority", sandbox->runtime) >= (int)sizeof(target))
        return hf_fail_msg("the Xauthority path is too long");
    bool copied;
    if (copy_runtime_file(sandbox, sandbox->xauthority, target, HF_ACCESS_FILE_RO, &copied) != 0) return 1;
    if (!copied) return 0;
    if (setenv("XAUTHORITY", target, 1) != 0) return hf_fail("setting XAUTHORITY");
    return 0;
}

static int bind_runtime_services(struct hf_sandbox *sandbox)
{
    if (bind_wayland(sandbox) != 0) return 1;
    if (bind_pipewire(sandbox) != 0) return 1;
    if (bind_pulse(sandbox) != 0) return 1;
    if (copy_xauthority(sandbox) != 0) return 1;
    return bind_x11(sandbox);
}

// J.1's container detection: pressure-vessel sets container=pressure-vessel and records the manager under
// /run/host/container-manager. Other container managers must not get /run/host exposed.
static bool container_runtime(void)
{
    const char *container = getenv("container");
    if (container != NULL && strcmp(container, "pressure-vessel") == 0) return true;
    char text[128];
    if (read_text("/run/host/container-manager", text, sizeof(text)) != 0) return false;
    return strstr(text, "pressure-vessel") != NULL;
}

static int bind_host_part(struct hf_sandbox *sandbox, const char *part)
{
    char source[PATH_MAX], target[PATH_MAX];
    if (snprintf(source, sizeof(source), "/run/host/%s", part) >= (int)sizeof(source) ||
        snprintf(target, sizeof(target), "/run/host/%s", part) >= (int)sizeof(target))
        return hf_fail_msg("a /run/host path is too long");
    struct stat info;
    if (stat(source, &info) != 0) return 0;
    if (!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode)) return 0;
    if (bind_readonly(sandbox, source, target) != 0) return 1;
    hf_note("binding the container's %s read-only", source);
    return 0;
}

// The container's /etc/ld.so.cache and ld.so.conf are links into pressure-vessel's generated loader folder. Without it
// the host loader finds no libc. Only that folder is bound, read-only; the rest of /var stays out of the view.
static int bind_container_ldso(struct hf_sandbox *sandbox)
{
    static const char ldso[] = "/var/pressure-vessel/ldso";
    struct stat info;
    if (lstat(ldso, &info) != 0 || !S_ISDIR(info.st_mode)) return 0;
    if (bind_readonly(sandbox, ldso, ldso) != 0) return 1;
    hf_note("binding the container's %s read-only", ldso);
    return 0;
}

// The container's loader and pressure-vessel overrides are links into /run/host. Bind the system parts read-only at
// their own paths; user-share, user-fonts-cache and etc are deliberately never bound whole.
static int bind_run_host(struct hf_sandbox *sandbox)
{
    static const char *const parts[] = { "usr", "lib", "lib64", "bin", "sbin", "share", "fonts", "os-release" };
    if (!container_runtime()) return 0;
    for (size_t index = 0; index < sizeof(parts) / sizeof(parts[0]); index++)
        if (bind_host_part(sandbox, parts[index]) != 0) return 1;
    return bind_container_ldso(sandbox);
}

static int mount_runtime(struct hf_sandbox *sandbox)
{
    if (mount_tmpfs(sandbox, "/run", "mode=0755,size=64m", HF_ACCESS_RO) != 0) return 1;
    if (create_runtime_dir(sandbox) != 0) return 1;
    if (bind_run_host(sandbox) != 0) return 1;
    if (bind_runtime_services(sandbox) != 0) return 1;
    char run[PATH_MAX];
    if (target_path(sandbox, "/run", run, sizeof(run)) != 0) return 1;
    if (chmod(run, 0555) != 0) return hf_fail("restricting /run");
    return 0;
}

// The Steam client pieces Proton and lsteamclient load, under the Steam root, when present.
static int bind_steam_client(struct hf_sandbox *sandbox)
{
    static const char *const parts[] = { "linux64", "linux32", "ubuntu12_32", "ubuntu12_64", "steamrt64" };
    if (sandbox->steam_root[0] == '\0') return 0;
    for (size_t index = 0; index < sizeof(parts) / sizeof(parts[0]); index++)
    {
        char source[PATH_MAX];
        struct stat info;
        if (snprintf(source, sizeof(source), "%s/%s", sandbox->steam_root, parts[index]) >= (int)sizeof(source))
            return hf_fail_msg("a Steam client path is too long");
        if (stat(source, &info) != 0 || !S_ISDIR(info.st_mode)) continue;
        if (bind_readonly(sandbox, source, source) != 0) return 1;
    }
    return 0;
}

static bool extra_bind_protected(const struct hf_sandbox *sandbox, const char *path);

// One --steam-runtime folder: the tool app the Proton's toolmanifest requires, resolved by the supervisor. It is bound
// read-only at its real path, and refused (fail closed) when it is /, covers the home, or overlaps the installation,
// the prefix or the session.
static int bind_one_steam_runtime(struct hf_sandbox *sandbox, const char *given)
{
    char resolved[PATH_MAX];
    struct stat info;
    if (realpath(given, resolved) == NULL || stat(resolved, &info) != 0 || !S_ISDIR(info.st_mode))
        return hf_fail_msg("the Steam runtime folder is missing: %s", given);
    if (strcmp(resolved, "/") == 0 || extra_bind_protected(sandbox, resolved) ||
        path_covers(sandbox->install, resolved) || path_covers(sandbox->prefix, resolved) ||
        path_covers(sandbox->session, resolved))
        return hf_fail_msg("the Steam runtime folder is not a separate tool folder: %s", resolved);
    return bind_readonly(sandbox, resolved, resolved);
}

static int bind_steam_runtime(struct hf_sandbox *sandbox)
{
    if (bind_steam_client(sandbox) != 0) return 1;
    for (int index = 0; index < sandbox->steam_runtime_count; index++)
        if (bind_one_steam_runtime(sandbox, sandbox->steam_runtimes[index]) != 0) return 1;
    return 0;
}

static int bind_vulkan_layers(struct hf_sandbox *sandbox)
{
    if (sandbox->vulkan_layers[0] == '\0') return 0;
    char target[PATH_MAX];
    if (hf_join3(target, sizeof(target), sandbox->data, "vulkan", "implicit_layer.d") != 0)
        return hf_fail("the Vulkan layer destination path");
    return bind_readonly(sandbox, sandbox->vulkan_layers, target);
}

static bool extra_bind_protected(const struct hf_sandbox *sandbox, const char *path)
{
    return path_covers(path, sandbox->install) || path_covers(path, sandbox->prefix) ||
        path_covers(path, sandbox->session) ||
        (sandbox->host_home[0] != '\0' && path_covers(path, sandbox->host_home));
}

static bool extra_bind_location(const struct hf_sandbox *sandbox, const char *path, mode_t mode)
{
    char mods[PATH_MAX], builds[PATH_MAX], session_host[PATH_MAX], loader[PATH_MAX];
    if (hf_join(mods, sizeof(mods), sandbox->install, "mods") != 0 ||
        hf_join(builds, sizeof(builds), sandbox->install, "temp/mod-builds") != 0 ||
        hf_join(session_host, sizeof(session_host), sandbox->install, "app/session") != 0 ||
        hf_join(loader, sizeof(loader), sandbox->install, "app/sdk-shims/hellforge_modloader.dll") != 0)
        return false;
    if (path_covers(mods, path) || path_covers(builds, path)) return true;
    if (strcmp(path, session_host) == 0) return S_ISDIR(mode);
    if (strcmp(path, loader) == 0) return S_ISREG(mode);
    return false;
}

// This allow list mirrors SandboxWire.AllowedBind in src/HellForge.Core/SandboxWire.cs. Change both together: the native
// side must reject a request the supervisor's validator would reject even if a caller bypasses that validator.
bool hf_extra_bind_allowed(const struct hf_sandbox *sandbox, const char *path)
{
    if (path == NULL || path[0] != '/') return false;
    char resolved[PATH_MAX];
    if (realpath(path, resolved) == NULL) return false;
    // realpath resolves every link, so a difference means a link in the given path (or a non-canonical spelling).
    if (strcmp(resolved, path) != 0) return false;
    struct stat info;
    if (lstat(path, &info) != 0) return false;
    if (S_ISLNK(info.st_mode)) return false;
    if (!S_ISDIR(info.st_mode) && !S_ISREG(info.st_mode)) return false;
    // Refuse the installation, prefix, session and real home themselves, and any folder above them.
    if (extra_bind_protected(sandbox, path)) return false;
    // Outside the installation is never an allowed input.
    if (!path_covers(sandbox->install, path)) return false;
    return extra_bind_location(sandbox, path, info.st_mode);
}

static int bind_extra_readonly(struct hf_sandbox *sandbox, const char *path)
{
    if (path == NULL) return hf_fail_msg("an extra read-only bind is missing its path");
    if (path[0] != '/') return hf_fail_msg("an extra read-only bind is not absolute: %s", path);
    if (sandbox->extra_bind_count > HF_MAX_EXTRA_BINDS) return hf_fail_msg("too many extra read-only binds");
    if (!hf_extra_bind_allowed(sandbox, path))
        return hf_fail_msg("an extra read-only bind is not an allowed link-free file or folder: %s", path);
    return bind_readonly(sandbox, path, path);
}

// Component-aware sort: '/' ranks below every other byte, so a folder and all its contents are adjacent.
static int compare_extra_bind_paths(const void *left, const void *right)
{
    const char *a = *(const char *const *)left;
    const char *b = *(const char *const *)right;
    while (*a != '\0' && *b != '\0')
    {
        unsigned char left_char = (unsigned char)*a;
        unsigned char right_char = (unsigned char)*b;
        if (left_char == right_char)
        {
            a++;
            b++;
            continue;
        }
        if (left_char == '/') return -1;
        if (right_char == '/') return 1;
        return left_char < right_char ? -1 : 1;
    }
    if (*a == *b) return 0;
    return *a == '\0' ? -1 : 1;
}

// The validated request's exact mod folders, source-built DLLs, loader and session host. Sort so a folder is mounted
// before its contents; a file already visible through an earlier folder bind is skipped with a note.
static int mount_extra_binds(struct hf_sandbox *sandbox)
{
    if (sandbox->extra_bind_count > HF_MAX_EXTRA_BINDS)
        return hf_fail_msg("too many extra read-only binds");
    for (int index = 0; index < sandbox->extra_bind_count; index++)
    {
        const char *path = sandbox->extra_binds[index];
        if (path == NULL) return hf_fail_msg("an extra read-only bind is missing its path");
        if (!hf_extra_bind_allowed(sandbox, path))
            return hf_fail_msg("an extra read-only bind is not an allowed link-free file or folder: %s", path);
    }
    if (sandbox->extra_bind_count > 1)
        qsort(sandbox->extra_binds, (size_t)sandbox->extra_bind_count, sizeof(sandbox->extra_binds[0]),
            compare_extra_bind_paths);
    const char *covering = NULL;
    for (int index = 0; index < sandbox->extra_bind_count; index++)
    {
        const char *path = sandbox->extra_binds[index];
        if (covering != NULL && path_covers(covering, path))
        {
            hf_note("extra read-only bind %s is covered by %s; skipping it", path, covering);
            continue;
        }
        if (bind_extra_readonly(sandbox, path) != 0) return 1;
        covering = path;
    }
    return 0;
}

static int mount_payload(struct hf_sandbox *sandbox)
{
    if (sandbox->proton_root[0] != '\0' && bind_readonly(sandbox, sandbox->proton_root, sandbox->proton_root) != 0) return 1;
    if (bind_proton_lock(sandbox) != 0) return 1;
    if (bind_steam_runtime(sandbox) != 0) return 1;
    if (sandbox->shader_cache[0] != '\0' && bind_writable_dir(sandbox, sandbox->shader_cache, sandbox->shader_cache) != 0) return 1;
    if (bind_readonly(sandbox, sandbox->game, sandbox->game) != 0) return 1;
    if (bind_writable_dir(sandbox, sandbox->game_out, sandbox->game_out) != 0) return 1;
    // The validated request's exact mod folders, source-built DLLs, loader and session host, at their real host paths.
    if (mount_extra_binds(sandbox) != 0) return 1;
    // Vulkan's implicit layer manifests live under XDG_DATA_HOME. Bind the host's folder read-only over the session's
    // data home; the manifest's libraries are under the Steam runtime paths bound above.
    if (bind_vulkan_layers(sandbox) != 0) return 1;
    // The overlay is a submount under gameOut. Add its own rule so Landlock covers the overlay root as well as the
    // plain directories under the session folder.
    if (hf_add_rule(sandbox, sandbox->merged, HF_ACCESS_RW) != 0) return 1;
    return 0;
}

int hf_build_root_view(struct hf_sandbox *sandbox)
{
    if (mount("tmpfs", sandbox->root, "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755,size=64m") != 0)
        return hf_fail("mounting the new root tmpfs");
    if (mount_tmpfs(sandbox, "/tmp", "mode=1777,size=2g", HF_ACCESS_RW) != 0) return 1;
    if (mount_system_dirs(sandbox) != 0) return 1;
    if (mount_proc_and_sys(sandbox) != 0) return 1;
    if (mount_dev(sandbox) != 0) return 1;
    if (mount_runtime(sandbox) != 0) return 1;
    if (mount_payload(sandbox) != 0) return 1;
    // Wine opens a path whose letter case differs from the disk (HELLDIVERS 2 opens its user_settings.config lowercased:
    // ".../documents/hellforge/...") by listing each parent folder to find the real name. The folders between the view's
    // mounts are bare tmpfs folders with no rule, so that listing failed with EACCES and the game started with default
    // settings, its first-time setup, every sandboxed session (measured 2026-10-29). Listing is the only right this adds:
    // the view's root is this tmpfs, so a listing shows only what the sandbox mounted and the folders leading to it, and
    // every bound folder could already be listed. Opening, reading and writing are unchanged.
    if (hf_add_rule(sandbox, "/", LANDLOCK_ACCESS_FS_READ_DIR) != 0) return 1;
    char old_root[PATH_MAX];
    if (target_path(sandbox, "/.oldroot", old_root, sizeof(old_root)) != 0) return 1;
    if (make_dir(old_root) != 0) return hf_fail("creating the old-root mount point");
    return 0;
}

int hf_pivot_into_root(struct hf_sandbox *sandbox)
{
    char old_root[PATH_MAX];
    if (target_path(sandbox, "/.oldroot", old_root, sizeof(old_root)) != 0) return 1;
    if (chdir(sandbox->root) != 0) return hf_fail("entering the new root");
    if (syscall(SYS_pivot_root, ".", ".oldroot") != 0) return hf_fail("pivot_root into the new root");
    if (chdir("/") != 0) return hf_fail("moving to the new root");
    if (umount2("/.oldroot", MNT_DETACH) != 0) return hf_fail("detaching the old root");
    if (rmdir("/.oldroot") != 0) return hf_fail("removing the old-root mount point");
    return 0;
}

static int set_env(const char *name, const char *value)
{
    if (setenv(name, value, 1) != 0) return hf_fail(name);
    return 0;
}

static int set_session_environment(struct hf_sandbox *sandbox)
{
    char wine_prefix[PATH_MAX];
    if (hf_join(wine_prefix, sizeof(wine_prefix), sandbox->merged, "pfx") != 0) return hf_fail("the overlay prefix path");
    // The real home, cache and temp are absent from the view; everything they would hold lands in the session folder.
    // WINEPREFIX and STEAM_COMPAT_DATA_PATH name the overlay, so Proton also gets a new prefix inode and a new wineserver.
    struct { const char *name; const char *value; } values[] =
    {
        { "HOME", sandbox->home },
        { "XDG_CACHE_HOME", sandbox->cache },
        { "XDG_CONFIG_HOME", sandbox->config },
        { "XDG_DATA_HOME", sandbox->data },
        { "XDG_STATE_HOME", sandbox->state },
        { "TMPDIR", "/tmp" },
        { "XDG_RUNTIME_DIR", sandbox->runtime },
        { "PYTHONDONTWRITEBYTECODE", "1" },
        { "WINEPREFIX", wine_prefix },
        { "STEAM_COMPAT_DATA_PATH", sandbox->merged },
    };
    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); index++)
        if (set_env(values[index].name, values[index].value) != 0) return 1;
    if (sandbox->steam_root[0] != '\0' && set_env("STEAM_COMPAT_CLIENT_INSTALL_PATH", sandbox->steam_root) != 0) return 1;
    return 0;
}

int hf_apply_environment(struct hf_sandbox *sandbox)
{
    if (set_session_environment(sandbox) != 0) return 1;
    // sandbox_canary.exe reads these; they are paths, not secrets, and let the test check the real home, prefix and game
    // copy without a second set of arguments.
    if (sandbox->host_home[0] != '\0' && set_env("HF_SANDBOX_REAL_HOME", sandbox->host_home) != 0) return 1;
    if (set_env("HF_SANDBOX_REAL_PREFIX", sandbox->prefix) != 0) return 1;
    if (set_env("HF_SANDBOX_GAME_COPY", sandbox->game) != 0) return 1;
    if (set_env("HF_SANDBOX_GAME_OUT", sandbox->game_out) != 0) return 1;
    // The runtime folder is the only session service view: Wayland, PipeWire, PulseAudio and X11 (with its cookie) are
    // bound above. D-Bus, portals and accessibility are deliberately not exposed; they are general-purpose control
    // channels and the four guarantees do not need them.
    if (unsetenv("DBUS_SESSION_BUS_ADDRESS") != 0) return hf_fail("clearing DBUS_SESSION_BUS_ADDRESS");
    return 0;
}

static int vulkan_library_path(const char *manifest, char *out, size_t size)
{
    out[0] = '\0';
    char text[8192];
    struct stat info;
    if (lstat(manifest, &info) != 0 || !S_ISREG(info.st_mode)) return 1;
    if (read_text(manifest, text, sizeof(text)) != 0) return 1;
    char *key = strstr(text, "\"library_path\"");
    if (key == NULL) return 0;
    char *colon = strchr(key, ':');
    char *quote = colon == NULL ? NULL : strchr(colon, '"');
    char *start = quote == NULL ? NULL : quote + 1;
    char *end = start == NULL ? NULL : strchr(start, '"');
    if (end == NULL) return 0;
    *end = '\0';
    if (start[0] == '/')
    {
        if (snprintf(out, size, "%s", start) >= (int)size) return 0;
        return 0;
    }
    char directory[PATH_MAX];
    if (snprintf(directory, sizeof(directory), "%s", manifest) >= (int)sizeof(directory)) return 0;
    char *slash = strrchr(directory, '/');
    if (slash == NULL) return 0;
    *slash = '\0';
    if (hf_join(out, size, directory, start) != 0) return 0;
    return 0;
}

// The manifests are bound read-only from the host's XDG_DATA_HOME. Read each one after the pivot and look for its
// library_path; a missing library means that layer (Fossilize shader recording, or the Steam overlay) will not load.
// This is diagnostic, not a reason to refuse the game: the four manifests on the measured host point into the Steam
// ubuntu12_32/64 folders, which are bound. A different manifest outside those bindings is logged for the reviewer.
int hf_check_vulkan_layers(const struct hf_sandbox *sandbox)
{
    if (sandbox->vulkan_layers[0] == '\0') return 0;
    char directory[PATH_MAX];
    if (hf_join3(directory, sizeof(directory), sandbox->data, "vulkan", "implicit_layer.d") != 0)
        return hf_fail("the Vulkan layer directory");
    DIR *layers = opendir(directory);
    if (layers == NULL) return hf_fail(directory);
    struct dirent *entry;
    while ((entry = readdir(layers)) != NULL)
    {
        const char *dot = strrchr(entry->d_name, '.');
        if (dot == NULL || strcmp(dot, ".json") != 0) continue;
        char manifest[PATH_MAX], library[PATH_MAX];
        if (hf_join(manifest, sizeof(manifest), directory, entry->d_name) != 0) continue;
        if (vulkan_library_path(manifest, library, sizeof(library)) != 0)
        {
            hf_note("Vulkan implicit layer %s is unreadable in the view", entry->d_name);
            continue;
        }
        if (library[0] != '\0' && access(library, R_OK) != 0)
            hf_note("Vulkan implicit layer %s: library_path %s is not in the view (%s)",
                entry->d_name, library, strerror(errno));
    }
    closedir(layers);
    return 0;
}
