// The game domain: session folders, the per-session overlay prefix and the namespaces. See hf-sandbox.h.

#include "hf-sandbox.h"

#include <linux/mount.h>

int hf_require_empty_dir(const char *path, const char *what)
{
    struct stat info;
    if (lstat(path, &info) != 0)
    {
        if (errno != ENOENT) return hf_fail(what);
        if (make_dir(path) != 0) return hf_fail(what);
        return 0;
    }
    if (!S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode))
        return hf_fail_msg("%s is not a plain folder: %s", what, path);
    DIR *directory = opendir(path);
    if (directory == NULL) return hf_fail(what);
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(directory)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        closedir(directory);
        return hf_fail_msg("%s is not empty: %s", what, path);
    }
    int read_error = errno;
    closedir(directory);
    if (read_error != 0) return hf_fail_msg("reading %s failed: %s (errno %d)", what, strerror(read_error), read_error);
    return 0;
}

static int make_session_dir(const char *path, const char *what)
{
    if (make_parent_dirs(path) != 0) return hf_fail(what);
    if (make_dir(path) != 0) return hf_fail(what);
    struct stat info;
    if (lstat(path, &info) != 0 || !S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode))
        return hf_fail_msg("%s is not a plain folder: %s", what, path);
    return 0;
}

int hf_create_session_dirs(struct hf_sandbox *sandbox)
{
    struct { const char *path; const char *what; } folders[] =
    {
        { sandbox->game_out, "the session's game folder" },
        { sandbox->overlay_root, "the overlay folder" },
        { sandbox->sandbox_state, "the sandbox state folder" },
        { sandbox->upper, "the overlay upper folder" },
        { sandbox->work, "the overlay work folder" },
        { sandbox->merged, "the overlay mount point" },
        { sandbox->home, "the session home" },
        { sandbox->cache, "the session cache" },
        { sandbox->config, "the session config" },
        { sandbox->data, "the session data" },
        { sandbox->state, "the session state" },
        { sandbox->root, "the new root folder" },
    };
    for (size_t index = 0; index < sizeof(folders) / sizeof(folders[0]); index++)
        if (make_session_dir(folders[index].path, folders[index].what) != 0) return 1;

    // A run folder is one session. An upper, work, merged or root folder with anything in it belongs to another run (or
    // an interrupted one); mixing them would reuse game-written data and could make the overlay serve stale files.
    if (hf_require_empty_dir(sandbox->upper, "the overlay upper folder") != 0) return 1;
    if (hf_require_empty_dir(sandbox->work, "the overlay work folder") != 0) return 1;
    if (hf_require_empty_dir(sandbox->merged, "the overlay mount point") != 0) return 1;
    if (hf_require_empty_dir(sandbox->root, "the new root folder") != 0) return 1;
    return 0;
}

static bool contains_dir(const char *base, const char *name, char *out, size_t size)
{
    struct stat info;
    if (hf_join(out, size, base, name) != 0) return false;
    return stat(out, &info) == 0 && S_ISDIR(info.st_mode);
}

static int make_steam_link(const char *target, const char *path)
{
    char existing[PATH_MAX];
    ssize_t length = readlink(path, existing, sizeof(existing) - 1);
    if (length >= 0)
    {
        existing[length] = '\0';
        if (strcmp(existing, target) == 0) return 0;
        return hf_fail_msg("the session home link %s points at %s, not %s", path, existing, target);
    }
    if (errno != ENOENT) return hf_fail(path);
    if (symlink(target, path) != 0) return hf_fail(path);
    return 0;
}

// lsteamclient opens $HOME/.steam/sdk64/steamclient.so (and the sdk32 sibling); on the host those are links into
// <Steam>/linux64 and linux32. Recreate only those two links in the session home before the pivot. Deliberately no
// root/steam aliases: the view binds only the runtime parts of Steam, never its config/ with the login tokens.
static int create_steam_dir(const struct hf_sandbox *sandbox, char *steam_dir, size_t size)
{
    if (hf_join(steam_dir, size, sandbox->home, ".steam") != 0) return hf_fail("the session .steam folder");
    if (make_parent_dirs(steam_dir) != 0 || make_dir(steam_dir) != 0) return hf_fail("creating the session .steam folder");
    struct stat info;
    if (lstat(steam_dir, &info) != 0 || !S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode))
        return hf_fail_msg("the session .steam path is not a plain folder: %s", steam_dir);
    return 0;
}

static int make_sdk_link(const struct hf_sandbox *sandbox, const char *steam_dir,
    const char *source_name, const char *link_name, const char *error)
{
    char source[PATH_MAX], link[PATH_MAX];
    if (!contains_dir(sandbox->steam_root, source_name, source, sizeof(source))) return 0;
    if (hf_join(link, sizeof(link), steam_dir, link_name) != 0) return hf_fail(error);
    return make_steam_link(source, link);
}

int hf_prepare_session_home(struct hf_sandbox *sandbox)
{
    if (sandbox->steam_root[0] == '\0') return 0;
    char steam_dir[PATH_MAX];
    if (create_steam_dir(sandbox, steam_dir, sizeof(steam_dir)) != 0) return 1;
    if (make_sdk_link(sandbox, steam_dir, "linux64", "sdk64", "the sdk64 link path") != 0) return 1;
    if (make_sdk_link(sandbox, steam_dir, "linux32", "sdk32", "the sdk32 link path") != 0) return 1;
    return 0;
}

int hf_mount_overlay(struct hf_sandbox *sandbox)
{
    size_t size = 4 * PATH_MAX;
    char *options = malloc(size);
    if (options == NULL) return hf_fail("allocating the overlay options");
    int written = snprintf(options, size, "lowerdir=%s,upperdir=%s,workdir=%s",
        sandbox->prefix, sandbox->upper, sandbox->work);
    if (written < 0 || (size_t)written >= size)
    {
        free(options);
        return hf_fail_msg("the overlay options are too long");
    }
    int mounted = mount("overlay", sandbox->merged, "overlay", MS_NOSUID | MS_NODEV, options);
    if (mounted != 0 && (errno == EPERM || errno == EINVAL))
    {
        // Unprivileged overlayfs on a filesystem that cannot hold trusted xattrs (ext4, btrfs without the option) needs
        // the user-namespace xattr namespace. This is the kernel's documented fallback, not a weaker sandbox.
        int saved = errno;
        written = snprintf(options, size, "lowerdir=%s,upperdir=%s,workdir=%s,userxattr",
            sandbox->prefix, sandbox->upper, sandbox->work);
        if (written < 0 || (size_t)written >= size)
        {
            free(options);
            return hf_fail_msg("the overlay options are too long");
        }
        errno = saved;
        mounted = mount("overlay", sandbox->merged, "overlay", MS_NOSUID | MS_NODEV, options);
    }
    free(options);
    if (mounted != 0) return hf_fail("mounting the per-session overlay prefix");

    char wine_prefix[PATH_MAX];
    if (hf_join(wine_prefix, sizeof(wine_prefix), sandbox->merged, "pfx") != 0)
        return hf_fail("the overlay's pfx path");
    struct stat info;
    if (stat(wine_prefix, &info) != 0 || !S_ISDIR(info.st_mode))
        return hf_fail_msg("the real prefix has no pfx/ folder; --prefix must be the Steam compatdata folder: %s", sandbox->prefix);
    hf_note("per-session overlay prefix at %s", sandbox->merged);
    return 0;
}

int hf_enter_namespaces(struct hf_sandbox *sandbox)
{
    (void)sandbox;
    int error = enter_user_namespace();
    if (error != 0) return hf_fail_msg("the user namespace: %s (errno %d)", strerror(-error), -error);
    if (syscall(SYS_unshare, CLONE_NEWNS) != 0) return hf_fail("the mount namespace");
    error = private_mount_tree();
    if (error != 0) return hf_fail_msg("making mount propagation private: %s (errno %d)", strerror(-error), -error);
    // Phase 3 adds CLONE_NEWPID, CLONE_NEWIPC and CLONE_NEWNET here (entered by a child, with a private /proc), and the
    // seccomp self-signal and process-creation rules. Phase 2 deliberately keeps the host PID, IPC and network views.
    return 0;
}
