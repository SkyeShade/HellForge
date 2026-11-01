// hf-sandbox --self-test: builds the same root view as a real session, applies the same guard, and runs the G1 canaries
// through it. Unlike the phase-1 probe this is not a toy view: the overlay, the read-only binds, the private /tmp and
// /dev/shm and the session's one writable folder are the real ones. The installation is built in the default layout
// (inside the real prefix at pfx/drive_c/users/steamuser/Documents/HellForge), so every expected absence exists on the
// host first and the overlay mask is exercised through both Z: and C:. Any unexpected success is a failure.

#include "hf-sandbox.h"

#include <stdarg.h>
#include <sys/socket.h>
#include <sys/un.h>

#define HOST_INSTALL "prefix/pfx/drive_c/users/steamuser/Documents/HellForge"
#define DELETED_CREDENTIAL_TEXT "deleted-credential-bytes"

// The self-test's scratch paths. The runtime fixture lives under /tmp because an AF_UNIX socket path is limited to
// sockaddr_un.sun_path (108 bytes); a worktree or installation path under it can be longer.
struct selftest_paths
{
    char temporary[PATH_MAX];
    char runtime[PATH_MAX];
    char escape_socket[PATH_MAX];
    char deleted_source[PATH_MAX];
    char deleted_target[PATH_MAX];
    char host_tmp[PATH_MAX];
    char host_shm[PATH_MAX];
};

static bool under(const char *ancestor, const char *path);
static int failures;

static void report(const char *name, bool passed, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    printf("%s %s: ", passed ? "PASS" : "FAIL", name);
    vprintf(format, arguments);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(arguments);
    if (!passed) failures++;
}

static bool try_write(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    bool ok = write_all(fd, text, strlen(text)) == 0;
    close(fd);
    return ok;
}

static void expect_denied(const char *name, const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
    {
        report(name, true, "%s: %s", path, strerror(errno));
        return;
    }
    close(fd);
    unlink(path);
    report(name, false, "%s was writable", path);
}

static void expect_allowed(const char *name, const char *path, const char *text)
{
    if (try_write(path, text)) report(name, true, "%s accepted a write", path);
    else report(name, false, "%s: %s", path, strerror(errno));
}

static void expect_missing(const char *name, const char *path)
{
    struct stat info;
    if (lstat(path, &info) == 0)
    {
        report(name, false, "%s is present", path);
        return;
    }
    if (errno == ENOENT) report(name, true, "%s is absent", path);
    else report(name, false, "%s: %s", path, strerror(errno));
}

static void expect_read(const char *name, const char *path, const char *expected)
{
    char text[128];
    if (read_text(path, text, sizeof(text)) == 0 && strcmp(text, expected) == 0)
        report(name, true, "%s is readable", path);
    else report(name, false, "%s: %s", path, strerror(errno));
}

struct game_copy_results
{
    int create, truncate;
    bool rename_ok, unlink_ok, mkdir_ok, link_ok, symlink_ok;
};

static int write_attempt(const char *path, int flags)
{
    int fd = open(path, flags | O_CLOEXEC, 0600);
    if (fd < 0) return errno;
    int result = write_all(fd, "canary", 6) == 0 ? 0 : errno;
    close(fd);
    return result;
}

// The full F.4a G1 operation set against the read-only game copy, which exists in the real view. Unlike a path that is
// simply absent, this exercises the read-only bind plus Landlock for every operation.
static void check_game_copy_operations(const struct hf_sandbox *sandbox)
{
    char keep[PATH_MAX], created[PATH_MAX], renamed[PATH_MAX], directory[PATH_MAX], hardlink[PATH_MAX], link_path[PATH_MAX];
    if (hf_join(keep, sizeof(keep), sandbox->game, "data/keep.txt") != 0 ||
        hf_join(created, sizeof(created), sandbox->game, "data/created") != 0 ||
        hf_join(renamed, sizeof(renamed), sandbox->game, "data/renamed") != 0 ||
        hf_join(directory, sizeof(directory), sandbox->game, "data/new-directory") != 0 ||
        hf_join(hardlink, sizeof(hardlink), sandbox->game, "data/hardlink") != 0 ||
        hf_join(link_path, sizeof(link_path), sandbox->game, "data/symlink") != 0)
    {
        report("game copy operations", false, "a canary path is too long");
        return;
    }
    struct game_copy_results result = { 0 };
    result.create = write_attempt(created, O_WRONLY | O_CREAT | O_TRUNC);
    int fd = open(keep, O_WRONLY | O_CLOEXEC);
    if (fd < 0) result.truncate = errno;
    else
    {
        result.truncate = ftruncate(fd, 0) == 0 ? 0 : errno;
        close(fd);
    }
    result.rename_ok = rename(keep, renamed) == 0;
    result.unlink_ok = unlink(keep) == 0;
    result.mkdir_ok = mkdir(directory, 0700) == 0;
    result.link_ok = link(keep, hardlink) == 0;
    result.symlink_ok = symlink("target", link_path) == 0;
    bool denied = result.create != 0 && result.truncate != 0 && !result.rename_ok && !result.unlink_ok &&
        !result.mkdir_ok && !result.link_ok && !result.symlink_ok;
    report("game copy operations", denied,
        "create/truncate/rename/unlink/mkdir/link/symlink: %s", denied ? "all denied" : "an operation succeeded");
}

static void check_private_mount(const char *name, const char *host_canary, const char *private_file)
{
    struct stat info;
    if (lstat(host_canary, &info) == 0) report(name, false, "the host canary is visible at %s", host_canary);
    else if (try_write(private_file, "private")) report(name, true, "the host canary is absent and %s is writable", private_file);
    else report(name, false, "writing %s: %s", private_file, strerror(errno));
}

// Wine finds a path given in another letter case (the game's lowercased user_settings.config) by listing each parent
// folder, so the view's bare folders must be listable. The listing must show only the view's own entries: the
// installation folder lists its logs folder (the way to the session) and never the host file planted beside it.
static void check_view_listing(const struct hf_sandbox *sandbox)
{
    DIR *directory = opendir(sandbox->install);
    if (directory == NULL)
    {
        report("view listing", false, "listing %s: %s", sandbox->install, strerror(errno));
        return;
    }
    const char *canary = strrchr(sandbox->selftest_listing_canary, '/');
    canary = canary == NULL ? sandbox->selftest_listing_canary : canary + 1;
    bool logs = false, leaked = false;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
    {
        if (strcmp(entry->d_name, "logs") == 0) logs = true;
        if (strcmp(entry->d_name, canary) == 0) leaked = true;
    }
    closedir(directory);
    if (leaked) report("view listing", false, "the host file %s is listed in the view", sandbox->selftest_listing_canary);
    else if (!logs) report("view listing", false, "%s was listed without its logs folder", sandbox->install);
    else report("view listing", true, "%s lists its own logs folder and not the host file %s", sandbox->install, canary);
}

static bool host_target_excluded(const char *path)
{
    static const char *const excluded[] =
        { "/run/host/user-share", "/run/host/user-fonts-cache", "/run/host/etc" };
    for (size_t index = 0; index < sizeof(excluded) / sizeof(excluded[0]); index++)
        if (under(excluded[index], path)) return true;
    return false;
}

static void check_host_link(const char *path, int *checked, int *excluded, int *missing)
{
    char target[PATH_MAX], resolved[PATH_MAX];
    ssize_t length = readlink(path, target, sizeof(target) - 1);
    if (length < 0) return;
    target[length] = '\0';
    const char *absolute = target;
    if (target[0] != '/')
    {
        if (realpath(path, resolved) == NULL) return;
        absolute = resolved;
    }
    if (!under("/run/host", absolute)) return;
    if (host_target_excluded(absolute))
    {
        (*excluded)++;
        printf("EXCLUDED %s -> %s\n", path, absolute);
        return;
    }
    (*checked)++;
    if (access(absolute, F_OK) != 0)
    {
        (*missing)++;
        printf("MISSING %s -> %s\n", path, absolute);
    }
}

static void walk_host_links(const char *root, int depth, int *checked, int *excluded, int *missing)
{
    if (depth > 8) return;
    DIR *directory = opendir(root);
    if (directory == NULL) return;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char path[PATH_MAX];
        if (hf_join(path, sizeof(path), root, entry->d_name) != 0) continue;
        struct stat info;
        if (lstat(path, &info) != 0) continue;
        if (S_ISDIR(info.st_mode))
        {
            walk_host_links(path, depth + 1, checked, excluded, missing);
            continue;
        }
        if (S_ISLNK(info.st_mode)) check_host_link(path, checked, excluded, missing);
    }
    closedir(directory);
}

// Every link in the container's loader/override folders must point at a /run/host target that the view either binds
// or deliberately excludes. The excluded paths are printed, never silently ignored.
static void check_run_host_links(void)
{
    if (access("/run/host", F_OK) != 0)
    {
        report("runtime /run/host links", true, "host mode: no /run/host");
        return;
    }
    static const char *const roots[] =
        { "/usr/lib/pressure-vessel", "/lib64", "/usr/lib64", "/usr/lib/x86_64-linux-gnu" };
    int checked = 0, excluded = 0, missing = 0;
    for (size_t index = 0; index < sizeof(roots) / sizeof(roots[0]); index++)
        walk_host_links(roots[index], 0, &checked, &excluded, &missing);
    if (missing == 0)
        report("runtime /run/host links", true, "%d target(s) bound, %d deliberately excluded", checked, excluded);
    else
        report("runtime /run/host links", false, "%d target(s) missing (%d bound, %d excluded)", missing, checked, excluded);
}

static void check_dynamic_program(void)
{
    pid_t child = fork();
    if (child < 0)
    {
        report("dynamic program", false, "fork: %s", strerror(errno));
        return;
    }
    if (child == 0)
    {
        execl("/bin/sh", "sh", "-c", "exit 0", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    bool finished = wait_bounded(child, 10, &status) == 1;
    if (!finished)
    {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
    }
    bool passed = finished && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    report("dynamic program", passed, "/bin/sh %s", passed ? "ran" : finished ? "failed" : "timed out");
}

static void check_runtime_sockets(const struct hf_sandbox *sandbox)
{
    static const char *const sockets[] = { "wayland-0", "pipewire-0", "pulse/native" };
    char path[PATH_MAX];
    struct stat info;
    for (size_t index = 0; index < sizeof(sockets) / sizeof(sockets[0]); index++)
    {
        char label[96];
        snprintf(label, sizeof(label), "runtime socket %s", sockets[index]);
        if (hf_join(path, sizeof(path), sandbox->runtime, sockets[index]) == 0 &&
            lstat(path, &info) == 0 && S_ISSOCK(info.st_mode))
            report(label, true, "%s is a socket", path);
        else report(label, false, "%s is missing or not a socket", path);
    }
    if (hf_join(path, sizeof(path), sandbox->runtime, "escape-0") == 0 && lstat(path, &info) != 0 && errno == ENOENT)
        report("runtime socket escape", true, "%s is absent", path);
    else report("runtime socket escape", false, "%s is present", path);
}

static void check_copied_credential(const struct hf_sandbox *sandbox)
{
    char path[PATH_MAX], text[64];
    struct stat info;
    if (hf_join(path, sizeof(path), sandbox->runtime, ".Xauthority") == 0 && stat(path, &info) == 0 &&
        S_ISREG(info.st_mode) && (info.st_mode & 0777) == 0400 && read_text(path, text, sizeof(text)) == 0 &&
        strcmp(text, DELETED_CREDENTIAL_TEXT) == 0)
        report("deleted credential copy", true, "%s has the copied bytes and mode 0400", path);
    else report("deleted credential copy", false, "%s did not hold the copied credential", path);
}

static void check_devices(void)
{
    const char *forbidden[] = { "/dev/input", "/dev/uinput", "/dev/kvm", "/dev/mem", "/dev/kmem" };
    bool passed = true;
    for (size_t index = 0; index < sizeof(forbidden) / sizeof(forbidden[0]); index++)
    {
        struct stat info;
        if (lstat(forbidden[index], &info) == 0)
        {
            passed = false;
            printf("FAIL minimal /dev: %s is visible\n", forbidden[index]);
            failures++;
        }
    }
    int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null < 0)
    {
        passed = false;
        printf("FAIL minimal /dev: /dev/null: %s\n", strerror(errno));
        failures++;
    }
    else close(null);
    if (passed) report("minimal /dev", true, "null works; input, uinput, kvm, mem and kmem are absent");
}

static void expect_denied_both(const char *label, const char *base, const char *name)
{
    char path[PATH_MAX];
    if (base[0] == '\0') return;
    if (hf_join(path, sizeof(path), base, name) != 0)
    {
        report(label, false, "a canary path is too long");
        return;
    }
    expect_missing(label, path);
    char write_label[96];
    snprintf(write_label, sizeof(write_label), "%s write", label);
    expect_denied(write_label, path);
}

static void check_denied_paths(const struct hf_sandbox *sandbox, const char *overlay_install)
{
    static const char *const sensitive[] =
    {
        "profiles/default/settings.ini", "mods/mod.txt", "userdata/save.dat",
        "account-snapshot/seal.key", "launcher-settings.json",
    };
    char label[96];
    for (size_t index = 0; index < sizeof(sensitive) / sizeof(sensitive[0]); index++)
    {
        snprintf(label, sizeof(label), "installation %s (Z:)", sensitive[index]);
        expect_denied_both(label, sandbox->install, sensitive[index]);
        snprintf(label, sizeof(label), "installation %s (C:)", sensitive[index]);
        expect_denied_both(label, overlay_install, sensitive[index]);
    }
    expect_denied_both("installation root (Z:)", sandbox->install, ".hf-sandbox-selftest");
    expect_denied_both("installation root (C:)", overlay_install, ".hf-sandbox-selftest");
    expect_denied_both("session root", sandbox->session, ".hf-sandbox-selftest");
    expect_denied_both("real home", sandbox->host_home, ".hf-sandbox-selftest");
    expect_denied_both("real prefix drive_c", sandbox->prefix, "pfx/drive_c/real-prefix-write");
    expect_denied_both("/etc", "/etc", ".hf-sandbox-selftest");
    expect_denied_both("/", "/", ".hf-sandbox-selftest");
}

static void check_overlay_game_copy(const struct hf_sandbox *sandbox)
{
    if (sandbox->overlay_install[0] == '\0')
    {
        report("overlay installation", false, "the installation was not detected inside the prefix");
        return;
    }
    char path[PATH_MAX];
    if (hf_join(path, sizeof(path), sandbox->overlay_install, "game/data/keep.txt") != 0)
    {
        report("game copy C:", false, "the path is too long");
        return;
    }
    expect_read("game copy C:", path, "game-copy");
    if (hf_join(path, sizeof(path), sandbox->game, "data/keep.txt") != 0)
    {
        report("game copy Z:", false, "the path is too long");
        return;
    }
    expect_read("game copy Z:", path, "game-copy");
}

static void check_seccomp_gaps(void)
{
    errno = 0;
    long result = syscall(SYS_clone, CLONE_NEWUSER | SIGCHLD, 0, 0, 0, 0);
    int error = errno;
    report("seccomp clone(CLONE_NEWUSER)", result == -1 && error == EPERM,
        "clone(CLONE_NEWUSER): result %ld, %s", result, strerror(error));
    errno = 0;
    result = syscall(SYS_clone3, NULL, 0);
    error = errno;
    report("seccomp clone3", result == -1 && error == ENOSYS, "clone3: result %ld, %s", result, strerror(error));
    errno = 0;
    result = syscall(SYS_getpid | __X32_SYSCALL_BIT);
    error = errno;
    report("seccomp x32", result == -1 && error == EPERM, "x32 getpid: result %ld, %s", result, strerror(error));
}

static void check_extra_bind_view(const struct hf_sandbox *sandbox)
{
    char folder[PATH_MAX], file[PATH_MAX], nested_folder[PATH_MAX], nested_file[PATH_MAX];
    if (hf_join3(folder, sizeof(folder), sandbox->install, "mods", "allowed") != 0 ||
        hf_join(file, sizeof(file), folder, "allowed.txt") != 0 ||
        hf_join3(nested_folder, sizeof(nested_folder), sandbox->install, "mods", "x") != 0 ||
        hf_join(nested_file, sizeof(nested_file), nested_folder, "x.dll") != 0)
    {
        report("extra bind", false, "a canary path is too long");
        return;
    }
    expect_read("extra bind read", file, "allowed");
    expect_denied("extra bind write", file);
    expect_read("extra bind nested read", nested_file, "allowed");
    expect_denied("extra bind nested write", nested_file);
}

static void run_canaries(const struct hf_sandbox *sandbox)
{
    char path[PATH_MAX];
    if (hf_join(path, sizeof(path), sandbox->game_out, "allowed.txt") == 0)
        expect_allowed("session write", path, "allowed");
    if (hf_join(path, sizeof(path), sandbox->game_out, "allowed-dir") == 0 && mkdir(path, 0700) == 0)
        report("session mkdir", true, "%s was created", path);
    else report("session mkdir", false, "%s: %s", path, strerror(errno));

    check_extra_bind_view(sandbox);
    check_denied_paths(sandbox, sandbox->overlay_install);
    check_game_copy_operations(sandbox);
    check_overlay_game_copy(sandbox);

    if (hf_join(path, sizeof(path), sandbox->merged, "pfx/drive_c/overlay-write") == 0)
        expect_allowed("overlay write", path, "overlay");

    check_view_listing(sandbox);
    check_private_mount("private /tmp", sandbox->selftest_tmp_canary, "/tmp/hf-sandbox-selftest-private");
    check_private_mount("private /dev/shm", sandbox->selftest_shm_canary, "/dev/shm/hf-sandbox-selftest-private");
    check_runtime_sockets(sandbox);
    check_copied_credential(sandbox);
    check_run_host_links();
    check_dynamic_program();
    check_devices();

    char text[64];
    if (read_text("/proc/self/status", text, sizeof(text)) == 0)
        report("proc readable", true, "/proc/self/status was readable");
    else report("proc readable", false, "/proc/self/status: %s", strerror(errno));
    check_seccomp_gaps();
}

static int make_tree_dir(const char *base, const char *relative)
{
    char path[PATH_MAX];
    if (hf_join(path, sizeof(path), base, relative) != 0) return hf_fail("a self-test path");
    if (make_parent_dirs(path) != 0 || make_dir(path) != 0) return hf_fail(path);
    return 0;
}

static int write_tree_file(const char *base, const char *relative, const char *text)
{
    char path[PATH_MAX];
    if (hf_join(path, sizeof(path), base, relative) != 0) return hf_fail("a self-test path");
    if (write_file(path, text) != 0) return hf_fail(path);
    return 0;
}

static bool under(const char *ancestor, const char *path)
{
    size_t length = strlen(ancestor);
    if (strncmp(ancestor, path, length) != 0) return false;
    return path[length] == '\0' || path[length] == '/';
}

static bool forbidden_base(const char *path)
{
    return under("/tmp", path) || under("/dev/shm", path) || under("/run", path) || under("/usr", path) || under("/etc", path);
}

static int copy_working_dir(char *out, size_t size)
{
    char working[PATH_MAX];
    if (getcwd(working, sizeof(working)) == NULL) return hf_fail("the self-test working folder");
    if (snprintf(out, size, "%s", working) >= (int)size) return hf_fail_msg("the self-test working path is too long");
    return 0;
}

static int copy_relative_dir(const char *directory, char *out, size_t size)
{
    char working[PATH_MAX];
    if (getcwd(working, sizeof(working)) == NULL) return hf_fail("the self-test working folder");
    if (snprintf(out, size, "%s/%s", working, directory) >= (int)size)
        return hf_fail_msg("the self-test directory is too long");
    return 0;
}

static int copy_test_dir_spec(const char *directory, char *out, size_t size)
{
    if (directory == NULL || directory[0] == '\0') return copy_working_dir(out, size);
    if (directory[0] == '/')
    {
        if (snprintf(out, size, "%s", directory) >= (int)size) return hf_fail_msg("the self-test directory is too long");
        return 0;
    }
    return copy_relative_dir(directory, out, size);
}

static int check_forbidden_base(const char *path)
{
    if (forbidden_base(path))
        return hf_fail_msg("--self-test-dir must not be under /tmp, /dev/shm, /run, /usr or /etc: %s", path);
    return 0;
}

static int resolve_test_base(char *out, size_t size)
{
    char *resolved = realpath(out, NULL);
    if (resolved == NULL) return hf_fail("the self-test directory");
    if (snprintf(out, size, "%s", resolved) >= (int)size)
    {
        free(resolved);
        return hf_fail_msg("the self-test directory is too long");
    }
    free(resolved);
    return 0;
}

static int self_test_base(const char *directory, char *out, size_t size)
{
    if (copy_test_dir_spec(directory, out, size) != 0) return 1;
    if (check_forbidden_base(out) != 0) return 1;
    if (make_parent_dirs(out) != 0 || make_dir(out) != 0) return hf_fail("the self-test directory");
    if (resolve_test_base(out, size) != 0) return 1;
    return check_forbidden_base(out);
}

static int make_socket_node(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return hf_fail("creating a self-test socket");
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (snprintf(address.sun_path, sizeof(address.sun_path), "%s", path) >= (int)sizeof(address.sun_path))
    {
        close(fd);
        return hf_fail_msg("a self-test socket path is too long: %s", path);
    }
    int result = bind(fd, (struct sockaddr *)&address, sizeof(address));
    int saved = errno;
    close(fd);
    if (result != 0) { errno = saved; return hf_fail(path); }
    return 0;
}

static int make_runtime_socket(struct selftest_paths *paths, const char *relative)
{
    char socket[PATH_MAX];
    if (hf_join(socket, sizeof(socket), paths->runtime, relative) != 0) return 1;
    return make_socket_node(socket);
}

static int make_runtime_link(struct selftest_paths *paths, const char *name, const char *target)
{
    char link[PATH_MAX];
    if (hf_join(link, sizeof(link), paths->runtime, name) != 0) return 1;
    if (symlink(target, link) != 0) return hf_fail_msg("creating the self-test link %s", name);
    return 0;
}

// A runtime folder with the pressure-vessel shape: wayland-0 and pulse are links to sockets in another folder inside
// the same runtime, pipewire-0 is a direct socket, and escape-0 is a link to a socket outside every allowed root.
static int prepare_runtime_sockets(struct selftest_paths *paths)
{
    if (make_tree_dir(paths->runtime, "real/pulse") != 0) return 1;
    if (make_runtime_socket(paths, "real/wayland-0") != 0) return 1;
    if (make_runtime_socket(paths, "pipewire-0") != 0) return 1;
    if (make_runtime_socket(paths, "real/pulse/native") != 0) return 1;
    if (snprintf(paths->escape_socket, sizeof(paths->escape_socket), "/tmp/hf-sandbox-selftest-escape-%ld",
        (long)getpid()) >= (int)sizeof(paths->escape_socket))
        return hf_fail_msg("the self-test escape socket path is too long");
    return make_socket_node(paths->escape_socket);
}

static int prepare_runtime_links(struct selftest_paths *paths)
{
    if (make_runtime_link(paths, "wayland-0", "real/wayland-0") != 0) return 1;
    if (make_runtime_link(paths, "pulse", "real/pulse") != 0) return 1;
    return make_runtime_link(paths, "escape-0", paths->escape_socket);
}

// The reviewer measured pressure-vessel's Xauthority as a bind mount of a deleted temp file: the path stays, the link
// count is 0, and mount(2) refuses it. The self-test leaves the same shape for the credential-copy path to read.
static int prepare_deleted_credential(struct selftest_paths *paths)
{
    if (hf_join(paths->deleted_source, sizeof(paths->deleted_source), paths->runtime, "deleted-source") != 0 ||
        hf_join(paths->deleted_target, sizeof(paths->deleted_target), paths->runtime, "deleted-target") != 0)
        return hf_fail("a self-test deleted-credential path");
    if (write_file(paths->deleted_source, DELETED_CREDENTIAL_TEXT) != 0 ||
        write_file(paths->deleted_target, "placeholder") != 0)
        return hf_fail("creating the self-test deleted-credential fixture");
    if (setenv("XAUTHORITY", paths->deleted_target, 1) != 0) return hf_fail("setting XAUTHORITY");
    return 0;
}

static int prepare_runtime_fixture(struct selftest_paths *paths)
{
    if (snprintf(paths->runtime, sizeof(paths->runtime), "/tmp/hf-sandbox-selftest-runtime.XXXXXX") >=
        (int)sizeof(paths->runtime))
        return hf_fail_msg("the self-test runtime path is too long");
    if (mkdtemp(paths->runtime) == NULL) return hf_fail("creating the self-test runtime folder");
    if (prepare_runtime_sockets(paths) != 0) return 1;
    if (prepare_runtime_links(paths) != 0) return 1;
    if (prepare_deleted_credential(paths) != 0) return 1;
    if (setenv("XDG_RUNTIME_DIR", paths->runtime, 1) != 0 || setenv("WAYLAND_DISPLAY", "wayland-0", 1) != 0)
        return hf_fail("setting the self-test runtime environment");
    unsetenv("PULSE_SERVER");
    unsetenv("DISPLAY");
    return 0;
}

static int prepare_tree(const char *directory, struct selftest_paths *paths)
{
    char base[PATH_MAX];
    if (self_test_base(directory, base, sizeof(base)) != 0) return 1;
    if (snprintf(paths->temporary, sizeof(paths->temporary), "%s/.hf-sandbox-selftest.XXXXXX", base) >=
        (int)sizeof(paths->temporary))
        return hf_fail_msg("the self-test temporary path is too long");
    if (mkdtemp(paths->temporary) == NULL) return hf_fail("creating the self-test temporary folder");
    const char *dirs[] =
    {
        HOST_INSTALL "/game/data", HOST_INSTALL "/profiles/default", HOST_INSTALL "/mods",
        HOST_INSTALL "/mods/allowed", HOST_INSTALL "/mods/x", HOST_INSTALL "/userdata", HOST_INSTALL "/account-snapshot", "outside",
    };
    for (size_t index = 0; index < sizeof(dirs) / sizeof(dirs[0]); index++)
        if (make_tree_dir(paths->temporary, dirs[index]) != 0) return 1;
    const struct { const char *name; const char *text; } files[] =
    {
        { HOST_INSTALL "/game/data/keep.txt", "game-copy" },
        { HOST_INSTALL "/hellforge-install.json", "{}" },
        { HOST_INSTALL "/launcher-settings.json", "{\"ui_scale\":1}" },
        { HOST_INSTALL "/profiles/default/settings.ini", "profile" },
        { HOST_INSTALL "/mods/mod.txt", "mod" },
        { HOST_INSTALL "/mods/allowed/allowed.txt", "allowed" },
        { HOST_INSTALL "/mods/x/x.dll", "allowed" },
        { HOST_INSTALL "/userdata/save.dat", "save" },
        { HOST_INSTALL "/account-snapshot/seal.key", "seal" },
        { "prefix/pfx/system.reg", "REAL-PREFIX" },
        { "outside/protected.txt", "outside-protected" },
    };
    for (size_t index = 0; index < sizeof(files) / sizeof(files[0]); index++)
        if (write_tree_file(paths->temporary, files[index].name, files[index].text) != 0) return 1;
    char mod_link[PATH_MAX];
    if (hf_join(mod_link, sizeof(mod_link), paths->temporary, "mod-link") != 0 ||
        symlink(HOST_INSTALL "/mods", mod_link) != 0)
        return hf_fail("creating the self-test bind link");
    snprintf(paths->host_tmp, sizeof(paths->host_tmp), "/tmp/hf-sandbox-selftest-host-%ld", (long)getpid());
    snprintf(paths->host_shm, sizeof(paths->host_shm), "/dev/shm/hf-sandbox-selftest-host-%ld", (long)getpid());
    // The host canaries make the private-mount checks meaningful. A host without /dev/shm can still run the self-test;
    // its private /dev/shm tmpfs is made by the view regardless.
    if (write_file(paths->host_tmp, "host-tmp") != 0) paths->host_tmp[0] = '\0';
    if (write_file(paths->host_shm, "host-shm") != 0) paths->host_shm[0] = '\0';
    return prepare_runtime_fixture(paths);
}

// Every expected denial must exist on the host before the view is built. This is the parent's check, before the fork.
static void check_host_tree(const char *temporary)
{
    static const char *const files[] =
    {
        HOST_INSTALL "/profiles/default/settings.ini", HOST_INSTALL "/mods/mod.txt",
        HOST_INSTALL "/userdata/save.dat", HOST_INSTALL "/account-snapshot/seal.key",
        HOST_INSTALL "/launcher-settings.json", HOST_INSTALL "/game/data/keep.txt",
        HOST_INSTALL "/hellforge-install.json", HOST_INSTALL "/mods/allowed/allowed.txt", HOST_INSTALL "/mods/x/x.dll",
    };
    bool present = true;
    char path[PATH_MAX];
    for (size_t index = 0; index < sizeof(files) / sizeof(files[0]); index++)
    {
        struct stat info;
        if (hf_join(path, sizeof(path), temporary, files[index]) != 0 || lstat(path, &info) != 0 || !S_ISREG(info.st_mode))
        {
            printf("FAIL host tree: missing %s\n", files[index]);
            present = false;
        }
    }
    if (present) report("host tree", true, "all denied paths exist on the host before the view is built");
    else failures++;
}

static void check_bind_rule(const char *name, const struct hf_sandbox *sandbox, const char *path, bool expected)
{
    bool actual = hf_extra_bind_allowed(sandbox, path);
    report(name, actual == expected, "%s: %s", path, actual ? "allowed" : "refused");
}

static void check_extra_bind_rules(const char *temporary, const struct hf_sandbox *sandbox)
{
    char allowed[PATH_MAX], nested_folder[PATH_MAX], nested_file[PATH_MAX];
    char link_path[PATH_MAX], linked[PATH_MAX], profiles[PATH_MAX];
    if (hf_join3(allowed, sizeof(allowed), sandbox->install, "mods", "allowed") != 0 ||
        hf_join3(nested_folder, sizeof(nested_folder), sandbox->install, "mods", "x") != 0 ||
        hf_join(nested_file, sizeof(nested_file), nested_folder, "x.dll") != 0 ||
        hf_join(link_path, sizeof(link_path), temporary, "mod-link") != 0 ||
        hf_join(linked, sizeof(linked), link_path, "allowed") != 0 ||
        hf_join(profiles, sizeof(profiles), sandbox->install, "profiles") != 0)
    {
        report("extra bind rules", false, "a canary path is too long");
        return;
    }
    struct stat link_info;
    if (lstat(link_path, &link_info) != 0 || !S_ISLNK(link_info.st_mode))
        report("extra bind link fixture", false, "%s is not a link", link_path);
    else check_bind_rule("extra bind link", sandbox, linked, false);
    check_bind_rule("extra bind installation", sandbox, sandbox->install, false);
    check_bind_rule("extra bind profiles", sandbox, profiles, false);
    check_bind_rule("extra bind mods", sandbox, allowed, true);
    check_bind_rule("extra bind nested mod", sandbox, nested_folder, true);
    check_bind_rule("extra bind nested file", sandbox, nested_file, true);
}

static bool verify_lower_tree(const char *temporary)
{
    char path[PATH_MAX], text[64];
    bool passed = true;
    if (hf_join(path, sizeof(path), temporary, "prefix/pfx/system.reg") != 0 ||
        read_text(path, text, sizeof(text)) != 0 || strcmp(text, "REAL-PREFIX") != 0)
    {
        printf("FAIL overlay lower: the real prefix changed\n");
        passed = false;
    }
    if (hf_join(path, sizeof(path), temporary, HOST_INSTALL "/game/data/keep.txt") != 0 ||
        read_text(path, text, sizeof(text)) != 0 || strcmp(text, "game-copy") != 0)
    {
        printf("FAIL game copy: the protected game file changed\n");
        passed = false;
    }
    const struct { const char *name; const char *text; } protected[] =
    {
        { HOST_INSTALL "/profiles/default/settings.ini", "profile" },
        { HOST_INSTALL "/mods/mod.txt", "mod" },
        { HOST_INSTALL "/userdata/save.dat", "save" },
        { HOST_INSTALL "/account-snapshot/seal.key", "seal" },
        { HOST_INSTALL "/launcher-settings.json", "{\"ui_scale\":1}" },
        { "outside/protected.txt", "outside-protected" },
    };
    for (size_t index = 0; index < sizeof(protected) / sizeof(protected[0]); index++)
    {
        if (hf_join(path, sizeof(path), temporary, protected[index].name) != 0 ||
            read_text(path, text, sizeof(text)) != 0 || strcmp(text, protected[index].text) != 0)
        {
            printf("FAIL host tree: %s changed\n", protected[index].name);
            passed = false;
        }
    }
    return passed;
}

static bool verify_canary_writes(const struct hf_sandbox *sandbox)
{
    char path[PATH_MAX], text[64];
    bool passed = true;
    if (hf_join(path, sizeof(path), sandbox->upper, "pfx/drive_c/overlay-write") != 0 ||
        read_text(path, text, sizeof(text)) != 0 || strcmp(text, "overlay") != 0)
    {
        printf("FAIL overlay upper: the write through the overlay did not land in the upper layer\n");
        passed = false;
    }
    if (hf_join(path, sizeof(path), sandbox->game_out, "allowed.txt") != 0 ||
        read_text(path, text, sizeof(text)) != 0 || strcmp(text, "allowed") != 0)
    {
        printf("FAIL session write: the allowed file did not survive\n");
        passed = false;
    }
    return passed;
}

static int verify_after(const char *temporary, const struct hf_sandbox *sandbox, bool canaries_ran)
{
    bool passed = verify_lower_tree(temporary);
    // A child that refused before the canaries has made no overlay or session writes, so those absences are not defects.
    if (canaries_ran && !verify_canary_writes(sandbox)) passed = false;
    if (passed && canaries_ran)
        printf("PASS overlay: lower unchanged; the upper layer and the session folder hold the writes\n");
    return passed ? 0 : 1;
}

static void clean_up(struct selftest_paths *paths)
{
    remove_tree(paths->temporary);
    remove_tree(paths->runtime);
    unlink(paths->escape_socket);
    unlink(paths->host_tmp);
    unlink(paths->host_shm);
}

// The parent resolves the fixture's links before the view exists: a link inside the runtime is accepted through its
// real path (the runtime socket fix), and a link that resolves outside every allowed root is dropped.
static void check_runtime_resolution(struct hf_sandbox *sandbox, const struct selftest_paths *paths)
{
    char expected[PATH_MAX], resolved[PATH_MAX];
    if (hf_join(expected, sizeof(expected), paths->runtime, "real/wayland-0") != 0 ||
        realpath(expected, resolved) == NULL)
    {
        report("runtime socket link", false, "the Wayland fixture is missing");
        return;
    }
    if (setenv("WAYLAND_DISPLAY", "escape-0", 1) != 0 || hf_discover_runtime_sockets(sandbox) != 0)
    {
        report("runtime socket escape link", false, "socket discovery failed");
        return;
    }
    report("runtime socket escape link", sandbox->wayland_socket[0] == '\0',
        "%s was %s", paths->escape_socket, sandbox->wayland_socket[0] == '\0' ? "dropped" : "bound");
    if (setenv("WAYLAND_DISPLAY", "wayland-0", 1) != 0 || hf_discover_runtime_sockets(sandbox) != 0)
    {
        report("runtime socket link", false, "socket discovery failed");
        return;
    }
    report("runtime socket link",
        strcmp(sandbox->wayland_socket, resolved) == 0 && strcmp(sandbox->wayland_name, "wayland-0") == 0,
        "%s -> %s as %s", paths->runtime, sandbox->wayland_socket, sandbox->wayland_name);
}

static int bind_deleted_source(const struct hf_sandbox *sandbox)
{
    if (sandbox->selftest_deleted_source[0] == '\0') return 0;
    if (mount(sandbox->selftest_deleted_source, sandbox->selftest_deleted_target, NULL, MS_BIND, NULL) != 0)
        return hf_fail("binding the self-test deleted-file fixture");
    if (unlink(sandbox->selftest_deleted_source) != 0)
        return hf_fail("unlinking the self-test deleted-file fixture");
    return 0;
}

static bool enter_selftest_view(struct hf_sandbox *sandbox)
{
    if (hf_enter_namespaces(sandbox) != 0 || bind_deleted_source(sandbox) != 0 ||
        hf_mount_overlay(sandbox) != 0 ||
        hf_mask_overlay_install(sandbox) != 0 || hf_build_root_view(sandbox) != 0 ||
        hf_pivot_into_root(sandbox) != 0 || hf_apply_environment(sandbox) != 0 ||
        hf_apply_landlock(sandbox) != 0 || hf_apply_seccomp() != 0 || hf_drop_capabilities() != 0)
        return false;
    return true;
}

static pid_t start_selftest_child(struct hf_sandbox *sandbox)
{
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0)
    {
        failures = 0;
        if (!enter_selftest_view(sandbox)) _exit(1);
        run_canaries(sandbox);
        _exit(failures == 0 ? 0 : 1);
    }
    return child;
}

static void wait_selftest_child(pid_t child, int *status, bool *finished)
{
    *finished = wait_bounded(child, 60, status) == 1;
    if (*finished) return;
    kill(-child, SIGKILL);
    kill(child, SIGKILL);
    waitpid(child, status, 0);
    printf("FAIL self-test: the sandbox child did not finish in 60 seconds\n");
}

static bool report_selftest(bool finished, int status, int after)
{
    bool passed = finished && WIFEXITED(status) && WEXITSTATUS(status) == 0 && after == 0;
    if (passed) printf("hf-sandbox: self-test PASS\n");
    else printf("hf-sandbox: self-test FAIL\n");
    fflush(stdout);
    return passed;
}

int hf_run_self_test(const char *directory)
{
    struct selftest_paths paths;
    memset(&paths, 0, sizeof(paths));
    if (prepare_tree(directory, &paths) != 0)
    {
        clean_up(&paths);
        return 1;
    }
    struct hf_sandbox sandbox;
    memset(&sandbox, 0, sizeof(sandbox));
    char allowed_bind[PATH_MAX] = "";
    char nested_folder_bind[PATH_MAX] = "";
    char nested_file_bind[PATH_MAX] = "";
    char *extra_binds[] = { nested_file_bind, allowed_bind, nested_folder_bind };
    snprintf(sandbox.selftest_tmp_canary, sizeof(sandbox.selftest_tmp_canary), "%s", paths.host_tmp);
    snprintf(sandbox.selftest_shm_canary, sizeof(sandbox.selftest_shm_canary), "%s", paths.host_shm);
    snprintf(sandbox.selftest_deleted_source, sizeof(sandbox.selftest_deleted_source), "%s", paths.deleted_source);
    snprintf(sandbox.selftest_deleted_target, sizeof(sandbox.selftest_deleted_target), "%s", paths.deleted_target);
    if (hf_prepare_selftest_paths(&sandbox, paths.temporary) != 0 || hf_create_session_dirs(&sandbox) != 0 ||
        hf_prepare_session_home(&sandbox) != 0 ||
        hf_join3(allowed_bind, sizeof(allowed_bind), sandbox.install, "mods", "allowed") != 0 ||
        hf_join3(nested_folder_bind, sizeof(nested_folder_bind), sandbox.install, "mods", "x") != 0 ||
        hf_join(nested_file_bind, sizeof(nested_file_bind), nested_folder_bind, "x.dll") != 0)
    {
        clean_up(&paths);
        return 1;
    }
    if (hf_join(sandbox.selftest_listing_canary, sizeof(sandbox.selftest_listing_canary), sandbox.install,
            ".hf-sandbox-listing-canary") != 0 || !try_write(sandbox.selftest_listing_canary, "host"))
    {
        hf_fail("the self-test's listing canary");
        clean_up(&paths);
        return 1;
    }
    sandbox.extra_binds = extra_binds;
    sandbox.extra_bind_count = 3;
    failures = 0;
    check_host_tree(paths.temporary);
    check_extra_bind_rules(paths.temporary, &sandbox);
    check_runtime_resolution(&sandbox, &paths);
    if (failures != 0)
    {
        clean_up(&paths);
        return 1;
    }

    pid_t child = start_selftest_child(&sandbox);
    if (child < 0)
    {
        hf_fail("forking the self-test");
        clean_up(&paths);
        return 1;
    }
    int status = 0;
    bool finished;
    wait_selftest_child(child, &status, &finished);
    bool canaries_ran = finished && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    int after = verify_after(paths.temporary, &sandbox, canaries_ran);
    clean_up(&paths);
    return report_selftest(finished, status, after) ? 0 : 1;
}
