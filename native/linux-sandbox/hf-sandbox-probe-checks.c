// Kernel probes and the small helpers shared by the two source files. See hf-sandbox-probe.h.

#include "hf-sandbox-probe.h"

uid_t host_uid;
gid_t host_gid;
char temp_root[PATH_MAX];
char protected_dir[PATH_MAX];
char protected_canary[PATH_MAX];
char session_dir[PATH_MAX];
char landlock_rw[PATH_MAX];
char tmp_canary[PATH_MAX];
char shm_canary[PATH_MAX];
volatile unsigned char *victim_buffer;

void set_result(struct shared_result *shared, int item, int status, const char *format, ...)
{
    if (item < 0 || item >= ITEM_COUNT) return;
    shared->status[item] = status;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(shared->evidence[item], sizeof(shared->evidence[item]), format, arguments);
    va_end(arguments);
}

bool make_shared_result(struct shared_result **shared_out)
{
    struct shared_result *shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED)
    {
        fprintf(stderr, "hf-sandbox-probe: FAIL: could not share the report between probe children\n");
        return false;
    }
    memset(shared, 0, sizeof(*shared));
    *shared_out = shared;
    return true;
}

bool make_victim_page(void)
{
    victim_buffer = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (victim_buffer == MAP_FAILED)
    {
        fprintf(stderr, "hf-sandbox-probe: FAIL: could not make the process-isolation canary page\n");
        return false;
    }
    memcpy((void *)victim_buffer, "victim-original", sizeof("victim-original"));
    return true;
}

void make_host_canaries(bool *have_tmp, bool *have_shm)
{
    snprintf(tmp_canary, sizeof(tmp_canary), "/tmp/hf-sandbox-probe-%ld-tmp", (long)getpid());
    *have_tmp = write_file(tmp_canary, "host-tmp") == 0;
    if (!*have_tmp) tmp_canary[0] = '\0';
    *have_shm = false;
    struct stat shm_info;
    if (stat("/dev/shm", &shm_info) == 0 && S_ISDIR(shm_info.st_mode))
    {
        snprintf(shm_canary, sizeof(shm_canary), "/dev/shm/hf-sandbox-probe-%ld", (long)getpid());
        *have_shm = write_file(shm_canary, "host-shm") == 0;
        if (!*have_shm) shm_canary[0] = '\0';
    }
}

// A bind of / is not read-only recursively by MS_REMOUNT alone: existing submounts (including /tmp or /home on many
// systems) keep their flags. mount_setattr(AT_RECURSIVE) closes that hole. The writable bind is made first, before the
// read-only lock; a bind made afterwards inherits a locked read-only mount in a user namespace and cannot be made
// writable again. The already-existing session mount has no such lock, so clearing its read-only attribute is allowed.
int make_root_readonly(const char *writable)
{
    if (mount(writable, writable, NULL, MS_BIND | MS_REC, NULL) != 0) return -errno;
    if (mount("/", "/", NULL, MS_BIND | MS_REC, NULL) != 0) return -errno;
    struct mount_attr attributes;
    memset(&attributes, 0, sizeof(attributes));
    attributes.attr_set = MOUNT_ATTR_RDONLY;
    if (syscall(SYS_mount_setattr, AT_FDCWD, "/", AT_RECURSIVE, &attributes, sizeof(attributes)) != 0)
        return -errno;
    if (mount(NULL, "/", NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) != 0) return -errno;
    memset(&attributes, 0, sizeof(attributes));
    attributes.attr_clr = MOUNT_ATTR_RDONLY;
    if (syscall(SYS_mount_setattr, AT_FDCWD, writable, 0, &attributes, sizeof(attributes)) != 0)
        return -errno;
    return 0;
}

void child_failure(struct shared_result *shared, int item, int code, const char *what)
{
    set_result(shared, item, STATUS_FAIL, "%s: %s (errno %d)", what, strerror(code), code);
}

int run_child(struct shared_result *shared, int item, int seconds, int (*probe)(struct shared_result *))
{
    pid_t pid = fork();
    if (pid < 0)
    {
        child_failure(shared, item, errno, "fork");
        return -1;
    }
    if (pid == 0)
    {
        setpgid(0, 0);
        alarm(seconds);
        int result = probe(shared);
        _exit(result == 0 ? 0 : 1);
    }
    setpgid(pid, pid);
    int status = 0;
    int waited = wait_bounded(pid, seconds + 2, &status);
    if (waited == 0)
    {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        if (shared->status[item] == STATUS_PENDING)
            set_result(shared, item, STATUS_FAIL, "the probe child did not finish within %d seconds", seconds + 2);
        return -1;
    }
    if (waited < 0 && shared->status[item] == STATUS_PENDING)
        set_result(shared, item, STATUS_FAIL, "waitpid failed: %s", strerror(errno));
    if (shared->status[item] == STATUS_PENDING)
    {
        if (WIFSIGNALED(status))
            set_result(shared, item, STATUS_FAIL, "the probe child was killed by signal %d", WTERMSIG(status));
        else
            set_result(shared, item, STATUS_FAIL, "the probe child exited with code %d without a result", WEXITSTATUS(status));
    }
    return shared->status[item] == STATUS_PASS ? 0 : -1;
}

int run_probe_child(struct shared_result *shared, int item, int (*probe)(struct shared_result *))
{
    return run_child(shared, item, CHILD_TIMEOUT_SECONDS, probe);
}

int probe_landlock(struct shared_result *shared)
{
    long abi = syscall(SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0)
    {
        set_result(shared, ITEM_LANDLOCK, STATUS_FAIL, "landlock_create_ruleset(VERSION): %s (errno %d)", strerror(errno), errno);
        return -1;
    }
    if (abi < 3)
    {
        set_result(shared, ITEM_LANDLOCK, STATUS_FAIL, "ABI %ld is below the required 3", abi);
        return -1;
    }
    set_result(shared, ITEM_LANDLOCK, STATUS_PASS, "ABI %ld (>= 3, includes TRUNCATE and REFER)", abi);
    return 0;
}

int probe_seccomp_filter_child(struct shared_result *shared)
{
    struct sock_filter filter[] =
    {
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog program = { .len = 1, .filter = filter };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    {
        set_result(shared, ITEM_SECCOMP_FILTER, STATUS_FAIL, "PR_SET_NO_NEW_PRIVS: %s", strerror(errno));
        return 1;
    }
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0)
    {
        set_result(shared, ITEM_SECCOMP_FILTER, STATUS_FAIL, "PR_SET_SECCOMP(FILTER): %s (errno %d)", strerror(errno), errno);
        return 1;
    }
    int mode = prctl(PR_GET_SECCOMP, 0, 0, 0, 0);
    if (mode != SECCOMP_MODE_FILTER)
    {
        set_result(shared, ITEM_SECCOMP_FILTER, STATUS_FAIL, "PR_GET_SECCOMP returned %d, expected %d", mode, SECCOMP_MODE_FILTER);
        return 1;
    }
    set_result(shared, ITEM_SECCOMP_FILTER, STATUS_PASS, "PR_GET_SECCOMP reports SECCOMP_MODE_FILTER (%d)", mode);
    return 0;
}

int probe_seccomp_user_notification(struct shared_result *shared)
{
    uint32_t action = SECCOMP_RET_USER_NOTIF;
    if (syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &action) != 0)
    {
        set_result(shared, ITEM_SECCOMP_USER_NOTIFICATION, STATUS_FAIL, "SECCOMP_GET_ACTION_AVAIL(USER_NOTIF): %s (errno %d)",
            strerror(errno), errno);
        return -1;
    }
    set_result(shared, ITEM_SECCOMP_USER_NOTIFICATION, STATUS_PASS, "SECCOMP_RET_USER_NOTIF is available");
    return 0;
}

int probe_userns_child(struct shared_result *shared)
{
    int error = enter_user_namespace();
    if (error != 0)
    {
        set_result(shared, ITEM_USERNS, STATUS_FAIL, "CLONE_NEWUSER with an unprivileged self-map: %s (errno %d)",
            strerror(-error), -error);
        return 1;
    }
    set_result(shared, ITEM_USERNS, STATUS_PASS, "CLONE_NEWUSER; host uid %u mapped to namespace uid 0", (unsigned)host_uid);
    return 0;
}

int probe_mountns_child(struct shared_result *shared)
{
    int error = enter_user_namespace();
    if (error != 0) { child_failure(shared, ITEM_MOUNTNS, -error, "user namespace before mount namespace"); return 1; }
    if (syscall(SYS_unshare, CLONE_NEWNS) != 0)
    {
        child_failure(shared, ITEM_MOUNTNS, errno, "CLONE_NEWNS");
        return 1;
    }
    error = private_mount_tree();
    if (error != 0) { child_failure(shared, ITEM_MOUNTNS, -error, "making mount propagation private"); return 1; }
    set_result(shared, ITEM_MOUNTNS, STATUS_PASS, "CLONE_NEWNS, mount propagation private");
    return 0;
}

int probe_pidns_child(struct shared_result *shared)
{
    int error = enter_user_namespace();
    if (error != 0) { child_failure(shared, ITEM_PIDNS, -error, "user namespace before PID namespace"); return 1; }
    if (syscall(SYS_unshare, CLONE_NEWPID) != 0)
    {
        child_failure(shared, ITEM_PIDNS, errno, "CLONE_NEWPID");
        return 1;
    }
    pid_t pid = fork();
    if (pid < 0) { child_failure(shared, ITEM_PIDNS, errno, "fork into PID namespace"); return 1; }
    if (pid == 0)
    {
        if (getpid() == 1)
            set_result(shared, ITEM_PIDNS, STATUS_PASS, "a child of CLONE_NEWPID has pid 1");
        else
            set_result(shared, ITEM_PIDNS, STATUS_FAIL, "the PID-namespace child has pid %ld, not 1", (long)getpid());
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (shared->status[ITEM_PIDNS] == STATUS_PENDING)
        set_result(shared, ITEM_PIDNS, STATUS_FAIL, "the PID-namespace child did not report");
    return shared->status[ITEM_PIDNS] == STATUS_PASS ? 0 : 1;
}

int probe_ipcns_child(struct shared_result *shared)
{
    int error = enter_user_namespace();
    if (error != 0) { child_failure(shared, ITEM_IPCNS, -error, "user namespace before IPC namespace"); return 1; }
    if (syscall(SYS_unshare, CLONE_NEWIPC) != 0)
    {
        child_failure(shared, ITEM_IPCNS, errno, "CLONE_NEWIPC");
        return 1;
    }
    set_result(shared, ITEM_IPCNS, STATUS_PASS, "CLONE_NEWIPC");
    return 0;
}

int probe_netns_child(struct shared_result *shared)
{
    int error = enter_user_namespace();
    if (error != 0) { child_failure(shared, ITEM_NETNS, -error, "user namespace before network namespace"); return 1; }
    if (syscall(SYS_unshare, CLONE_NEWNET) != 0)
    {
        child_failure(shared, ITEM_NETNS, errno, "CLONE_NEWNET");
        return 1;
    }
    int socket_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0) { child_failure(shared, ITEM_NETNS, errno, "socket in the network namespace"); return 1; }
    close(socket_fd);
    unsigned loopback = if_nametoindex("lo");
    if (loopback == 0)
    {
        set_result(shared, ITEM_NETNS, STATUS_FAIL, "CLONE_NEWNET succeeded but loopback has no index");
        return 1;
    }
    set_result(shared, ITEM_NETNS, STATUS_PASS, "CLONE_NEWNET; private loopback index %u", loopback);
    return 0;
}

struct overlay_paths
{
    char *storage;
    char *options;
    char *lower;
    char *upper;
    char *work;
    char *merged;
    char *lower_file;
    char *merged_file;
    char *merged_new;
};

// The paths and the mount options are heap buffers: seven PATH_MAX arrays plus the options would exceed the
// repository's per-function stack limit.
static bool overlay_paths_init(struct overlay_paths *paths)
{
    paths->storage = malloc(7 * PATH_MAX);
    paths->options = malloc(4 * PATH_MAX);
    if (paths->storage == NULL || paths->options == NULL)
    {
        free(paths->storage);
        free(paths->options);
        paths->storage = NULL;
        paths->options = NULL;
        return false;
    }
    paths->lower = paths->storage;
    paths->upper = paths->lower + PATH_MAX;
    paths->work = paths->upper + PATH_MAX;
    paths->merged = paths->work + PATH_MAX;
    paths->lower_file = paths->merged + PATH_MAX;
    paths->merged_file = paths->lower_file + PATH_MAX;
    paths->merged_new = paths->merged_file + PATH_MAX;
    joinf(paths->lower, PATH_MAX, temp_root, "overlay-lower");
    joinf(paths->upper, PATH_MAX, temp_root, "overlay-upper");
    joinf(paths->work, PATH_MAX, temp_root, "overlay-work");
    joinf(paths->merged, PATH_MAX, temp_root, "overlay-merged");
    joinf(paths->lower_file, PATH_MAX, paths->lower, "original.txt");
    joinf(paths->merged_file, PATH_MAX, paths->merged, "original.txt");
    joinf(paths->merged_new, PATH_MAX, paths->merged, "new.txt");
    return true;
}

static void overlay_paths_free(struct overlay_paths *paths)
{
    free(paths->options);
    free(paths->storage);
}

static bool overlay_prepare(const struct overlay_paths *paths, int *saved, const char **failure)
{
    if (make_dir(paths->lower) != 0 || make_dir(paths->upper) != 0 ||
        make_dir(paths->work) != 0 || make_dir(paths->merged) != 0)
    {
        *saved = errno;
        *failure = "creating overlay folders";
        return false;
    }
    if (write_file(paths->lower_file, "lower-original") != 0)
    {
        *saved = errno;
        *failure = "writing lower file";
        return false;
    }
    if (chmod(paths->lower, 0500) != 0)
    {
        *saved = errno;
        *failure = "making the lower layer read-only";
        return false;
    }
    return true;
}

static bool overlay_exercise(struct shared_result *shared, const struct overlay_paths *paths, bool *mounted,
    int *saved, const char **failure)
{
    snprintf(paths->options, 4 * PATH_MAX, "lowerdir=%s,upperdir=%s,workdir=%s",
        paths->lower, paths->upper, paths->work);
    if (mount("overlay", paths->merged, "overlay", MS_NOSUID | MS_NODEV, paths->options) != 0)
    {
        *saved = errno;
        *failure = "mount overlay in the user namespace";
        return false;
    }
    *mounted = true;
    if (write_file(paths->merged_new, "upper-new") != 0)
    {
        *saved = errno;
        *failure = "writing through the overlay";
        return false;
    }
    if (write_file(paths->merged_file, "upper-changed") != 0)
    {
        *saved = errno;
        *failure = "copy-up through the overlay";
        return false;
    }
    char original[64];
    if (read_text(paths->lower_file, original, sizeof(original)) != 0 ||
        strcmp(original, "lower-original") != 0)
    {
        set_result(shared, ITEM_OVERLAYFS, STATUS_FAIL, "the lower layer changed when the overlay was written");
        return false;
    }
    set_result(shared, ITEM_OVERLAYFS, STATUS_PASS,
        "overlay mount in a user namespace; upper write and copy-up worked; lower unchanged");
    return true;
}

int probe_overlayfs_child(struct shared_result *shared)
{
    int error = enter_user_namespace();
    if (error != 0) { child_failure(shared, ITEM_OVERLAYFS, -error, "user namespace before overlayfs"); return 1; }
    if (syscall(SYS_unshare, CLONE_NEWNS) != 0) { child_failure(shared, ITEM_OVERLAYFS, errno, "CLONE_NEWNS"); return 1; }
    error = private_mount_tree();
    if (error != 0) { child_failure(shared, ITEM_OVERLAYFS, -error, "making mount propagation private"); return 1; }

    struct overlay_paths paths;
    if (!overlay_paths_init(&paths))
    {
        set_result(shared, ITEM_OVERLAYFS, STATUS_FAIL, "could not allocate the overlay paths");
        return 1;
    }

    int result = 1;
    int saved = 0;
    const char *failure = NULL;
    bool mounted = false;
    if (!overlay_prepare(&paths, &saved, &failure)) goto done;
    if (!overlay_exercise(shared, &paths, &mounted, &saved, &failure)) goto done;
    result = 0;

done:
    if (mounted) umount2(paths.merged, MNT_DETACH);
    if (failure != NULL) child_failure(shared, ITEM_OVERLAYFS, saved, failure);
    overlay_paths_free(&paths);
    return result;
}

int probe_readonly_root_child(struct shared_result *shared)
{
    int error = enter_user_namespace();
    if (error != 0) { child_failure(shared, ITEM_READONLY_ROOT, -error, "user namespace before read-only root"); return 1; }
    if (syscall(SYS_unshare, CLONE_NEWNS) != 0) { child_failure(shared, ITEM_READONLY_ROOT, errno, "CLONE_NEWNS"); return 1; }
    error = private_mount_tree();
    if (error != 0) { child_failure(shared, ITEM_READONLY_ROOT, -error, "making mount propagation private"); return 1; }

    error = make_root_readonly(session_dir);
    if (error != 0)
    {
        child_failure(shared, ITEM_READONLY_ROOT, -error, "building the read-only root with its writable session bind");
        return 1;
    }

    char allowed_file[PATH_MAX];
    joinf(allowed_file, sizeof(allowed_file), session_dir, "readonly-root-allowed.txt");
    if (write_file(allowed_file, "allowed") != 0)
    {
        child_failure(shared, ITEM_READONLY_ROOT, errno, "writing the writable session bind");
        return 1;
    }
    int fd = open(protected_canary, O_WRONLY | O_CLOEXEC);
    if (fd >= 0)
    {
        close(fd);
        set_result(shared, ITEM_READONLY_ROOT, STATUS_FAIL, "a write outside the session folder succeeded on the read-only root");
        return 1;
    }
    int saved = errno;
    if (saved != EROFS && saved != EACCES && saved != EPERM)
    {
        child_failure(shared, ITEM_READONLY_ROOT, saved, "unexpected write error outside the session folder");
        return 1;
    }
    set_result(shared, ITEM_READONLY_ROOT, STATUS_PASS, "protected open: %s; session bind write: OK", strerror(saved));
    return 0;
}

// /tmp and /dev/shm use the same check; only the mount path and the host-side canary differ.
static int probe_private_mount(struct shared_result *shared, int item, const char *path, const char *host_canary)
{
    char message[160];
    snprintf(message, sizeof(message), "user namespace before private %s", path);
    int error = enter_user_namespace();
    if (error != 0) { child_failure(shared, item, -error, message); return 1; }
    if (syscall(SYS_unshare, CLONE_NEWNS) != 0) { child_failure(shared, item, errno, "CLONE_NEWNS"); return 1; }
    error = private_mount_tree();
    if (error != 0) { child_failure(shared, item, -error, "making mount propagation private"); return 1; }
    if (mount("tmpfs", path, "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777,size=64m") != 0)
    {
        snprintf(message, sizeof(message), "mounting a private %s", path);
        child_failure(shared, item, errno, message);
        return 1;
    }
    if (path_exists(host_canary))
    {
        set_result(shared, item, STATUS_FAIL, "the host %s canary is still visible in the private mount", path);
        return 1;
    }
    char private_file[PATH_MAX];
    joinf(private_file, sizeof(private_file), path, "hf-sandbox-probe-private");
    if (write_file(private_file, "private") != 0)
    {
        snprintf(message, sizeof(message), "writing the private %s", path);
        child_failure(shared, item, errno, message);
        return 1;
    }
    set_result(shared, item, STATUS_PASS, "tmpfs on %s; the host canary is absent and a private write works", path);
    return 0;
}

int probe_private_tmp_child(struct shared_result *shared)
{
    return probe_private_mount(shared, ITEM_PRIVATE_TMP, "/tmp", tmp_canary);
}

int probe_private_shm_child(struct shared_result *shared)
{
    return probe_private_mount(shared, ITEM_PRIVATE_SHM, "/dev/shm", shm_canary);
}

int probe_openat2(struct shared_result *shared)
{
    int directory = open(temp_root, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0)
    {
        child_failure(shared, ITEM_OPENAT2, errno, "opening the temporary folder");
        return -1;
    }
    char file[PATH_MAX];
    joinf(file, sizeof(file), temp_root, "openat2-target.txt");
    if (write_file(file, "openat2") != 0)
    {
        int saved = errno;
        close(directory);
        child_failure(shared, ITEM_OPENAT2, saved, "creating the openat2 target");
        return -1;
    }
    struct open_how how;
    memset(&how, 0, sizeof(how));
    how.flags = O_RDONLY | O_CLOEXEC;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS;
    int fd = (int)syscall(SYS_openat2, directory, "openat2-target.txt", &how, sizeof(how));
    int saved = errno;
    close(directory);
    if (fd < 0)
    {
        set_result(shared, ITEM_OPENAT2, STATUS_FAIL, "openat2: %s (errno %d)", strerror(saved), saved);
        return -1;
    }
    char text[32];
    ssize_t got = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (got < 0) got = 0;
    text[got] = '\0';
    if (strcmp(text, "openat2") != 0)
    {
        set_result(shared, ITEM_OPENAT2, STATUS_FAIL, "openat2 returned the wrong contents");
        return -1;
    }
    set_result(shared, ITEM_OPENAT2, STATUS_PASS, "openat2 opened a relative regular file with RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS");
    return 0;
}

int probe_close_range(struct shared_result *shared)
{
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        child_failure(shared, ITEM_CLOSE_RANGE, errno, "opening /dev/null");
        return -1;
    }
    int first = fd;
    if (syscall(SYS_close_range, (unsigned)first, (unsigned)first, 0) != 0)
    {
        int saved = errno;
        close(first);
        set_result(shared, ITEM_CLOSE_RANGE, STATUS_FAIL, "close_range: %s (errno %d)", strerror(saved), saved);
        return -1;
    }
    if (fcntl(first, F_GETFD) != -1 || errno != EBADF)
    {
        close(first);
        set_result(shared, ITEM_CLOSE_RANGE, STATUS_FAIL, "close_range returned success but did not close the descriptor");
        return -1;
    }
    set_result(shared, ITEM_CLOSE_RANGE, STATUS_PASS, "close_range closed the named descriptor");
    return 0;
}

int probe_no_new_privs_child(struct shared_result *shared)
{
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    {
        set_result(shared, ITEM_NO_NEW_PRIVS, STATUS_FAIL, "PR_SET_NO_NEW_PRIVS: %s (errno %d)", strerror(errno), errno);
        return 1;
    }
    int value = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
    if (value != 1)
    {
        set_result(shared, ITEM_NO_NEW_PRIVS, STATUS_FAIL, "PR_GET_NO_NEW_PRIVS returned %d", value);
        return 1;
    }
    set_result(shared, ITEM_NO_NEW_PRIVS, STATUS_PASS, "PR_SET_NO_NEW_PRIVS=1 and PR_GET_NO_NEW_PRIVS=1");
    return 0;
}

int probe_apparmor(struct shared_result *shared)
{
    char value[64];
    if (read_text("/proc/sys/kernel/apparmor_restrict_unprivileged_userns", value, sizeof(value)) != 0)
    {
        if (errno == ENOENT)
        {
            set_result(shared, ITEM_APPARMOR, STATUS_PASS, "the AppArmor limitation sysctl does not exist");
            return 0;
        }
        set_result(shared, ITEM_APPARMOR, STATUS_SKIP, "cannot read the AppArmor limitation sysctl: %s (errno %d)", strerror(errno), errno);
        return -1;
    }
    char *end = value + strlen(value);
    while (end > value && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
    char *at = value;
    while (*at == ' ' || *at == '\t') at++;
    if (*at == '1')
    {
        char profile[256];
        if (read_text("/proc/self/attr/current", profile, sizeof(profile)) == 0 && strstr(profile, "hellforge-unshare") != NULL)
        {
            set_result(shared, ITEM_APPARMOR, STATUS_PASS,
                "apparmor_restrict_unprivileged_userns=1, but this probe runs under the hellforge-unshare profile that allow-namespace.sh installed");
            return 0;
        }
        set_result(shared, ITEM_APPARMOR, STATUS_SKIP,
            "apparmor_restrict_unprivileged_userns=1; run allow-namespace.sh, then run the probe again through the wrapper");
        return -1;
    }
    set_result(shared, ITEM_APPARMOR, STATUS_PASS, "apparmor_restrict_unprivileged_userns=%s", value);
    return 0;
}
