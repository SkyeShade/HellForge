// The canaries, report and entry point of HellForge's Linux sandbox probe (review-F1.md phase 1).
//
// This program never installs or changes anything. It probes the unprivileged kernel mechanisms the Linux sandbox
// design needs, then runs the four canaries in throwaway namespace children: writes outside the session folder,
// process-memory access and signals to an outside process, starting another program, and reaching a loopback port that
// exists only outside the child. Every item is PASS, FAIL or SKIP, and the JSON report says what was observed.
//
// Build (the Linux package workflow runs the same command with the pinned Zig):
//   zig cc -target x86_64-linux-musl -static -O2 -Wall -Werror native/linux-sandbox/*.c -o hf-sandbox-probe
//
// Usage: hf-sandbox-probe [--json <file>]
// Exit 0 only when every mechanism is present and every canary held.

#include "hf-sandbox-probe.h"

struct item_def
{
    const char *id;
    const char *group;
    const char *title;
};

static const struct item_def ITEMS[ITEM_COUNT] =
{
    { "landlock", "mechanism", "Landlock ABI >= 3" },
    { "seccomp_filter", "mechanism", "seccomp filter mode" },
    { "seccomp_user_notification", "mechanism", "seccomp user notification" },
    { "userns", "mechanism", "unprivileged user namespace" },
    { "mountns", "mechanism", "mount namespace" },
    { "pidns", "mechanism", "PID namespace" },
    { "ipcns", "mechanism", "IPC namespace" },
    { "netns", "mechanism", "network namespace" },
    { "overlayfs_userns", "mechanism", "overlayfs in a user namespace" },
    { "readonly_root", "mechanism", "read-only / with a writable session bind" },
    { "private_tmp", "mechanism", "private /tmp" },
    { "private_shm", "mechanism", "private /dev/shm" },
    { "openat2", "mechanism", "openat2 with RESOLVE_BENEATH and RESOLVE_NO_SYMLINKS" },
    { "close_range", "mechanism", "close_range" },
    { "no_new_privs", "mechanism", "PR_SET_NO_NEW_PRIVS" },
    { "apparmor_userns", "mechanism", "AppArmor unprivileged user-namespace restriction" },
    { "g1", "canary", "G1: writes outside the session folders fail" },
    { "g1_landlock", "canary", "G1: Landlock denies a writable non-session mount" },
    { "g1_allowed", "canary", "G1: writes in the session folder succeed" },
    { "g2", "canary", "G2: ptrace, process_vm_writev and signals to an outside process fail" },
    { "g2_self_signal", "canary", "G2: a signal to the canary itself succeeds" },
    { "g3", "canary", "G3: fork, vfork, clone without CLONE_THREAD, clone3 and execve fail" },
    { "g3_threads", "canary", "G3: threads still start" },
    { "g4", "canary", "G4: an outside loopback TCP port is unreachable" },
};

static const char *status_name(int status)
{
    switch (status)
    {
        case STATUS_PASS: return "PASS";
        case STATUS_FAIL: return "FAIL";
        case STATUS_SKIP: return "SKIP";
        default: return "PENDING";
    }
}

static int apply_landlock(struct shared_result *shared, int item)
{
    uint64_t access = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |
        LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |
        LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
        LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_REFER | LANDLOCK_ACCESS_FS_TRUNCATE;
    struct landlock_ruleset_attr ruleset = { .handled_access_fs = access };
    int ruleset_fd = (int)syscall(SYS_landlock_create_ruleset, &ruleset, sizeof(ruleset), 0);
    if (ruleset_fd < 0)
    {
        set_result(shared, item, STATUS_FAIL, "landlock_create_ruleset: %s (errno %d)", strerror(errno), errno);
        return -1;
    }
    int session_fd = open(session_dir, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (session_fd < 0)
    {
        int saved = errno;
        close(ruleset_fd);
        child_failure(shared, item, saved, "opening the session folder for Landlock");
        return -1;
    }
    struct landlock_path_beneath_attr beneath = { .allowed_access = access, .parent_fd = session_fd };
    if (syscall(SYS_landlock_add_rule, ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &beneath, 0) != 0)
    {
        int saved = errno;
        close(session_fd);
        close(ruleset_fd);
        child_failure(shared, item, saved, "LANDLOCK_RULE_PATH_BENEATH");
        return -1;
    }
    close(session_fd);
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    {
        int saved = errno;
        close(ruleset_fd);
        child_failure(shared, item, saved, "PR_SET_NO_NEW_PRIVS before Landlock");
        return -1;
    }
    if (syscall(SYS_landlock_restrict_self, ruleset_fd, 0) != 0)
    {
        int saved = errno;
        close(ruleset_fd);
        child_failure(shared, item, saved, "landlock_restrict_self");
        return -1;
    }
    close(ruleset_fd);
    return 0;
}

static int loopback_up(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct ifreq request;
    memset(&request, 0, sizeof(request));
    strncpy(request.ifr_name, "lo", IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFFLAGS, &request) != 0)
    {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    request.ifr_flags = (short)(request.ifr_flags | IFF_UP | IFF_RUNNING);
    if (ioctl(fd, SIOCSIFFLAGS, &request) != 0)
    {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    close(fd);
    return 0;
}

static volatile sig_atomic_t self_signal_seen;

static void self_signal_handler(int signal_number)
{
    (void)signal_number;
    self_signal_seen = 1;
}

static void filter_jump_to(struct sock_filter *filter, size_t jump, size_t target, bool on_true)
{
    int offset = (int)target - (int)jump - 1;
    if (on_true) filter[jump].jt = (unsigned char)offset;
    else filter[jump].jf = (unsigned char)offset;
}

static int install_canary_seccomp(pid_t self_pid, pid_t self_tid)
{
    struct sock_filter filter[MAX_FILTER];
    size_t length = hf_filter_begin(filter, MAX_FILTER);
    // The shared prelude denies x32 and clone3 with ENOSYS. This shared block denies clone with any CLONE_NEW* bit and
    // restores A=nr, so the phase-3 clone/thread and kill rules below are unchanged.
    if (hf_filter_deny_clone_namespaces(filter, &length, MAX_FILTER) != 0) return -1;

#define DENY_SYSCALL(number) hf_filter_deny(filter, &length, (unsigned int)(number))

    // The phase-3 process and memory denials, on top of the mount/namespace set hf_filter_begin already added.
    DENY_SYSCALL(SYS_ptrace);
    DENY_SYSCALL(SYS_process_vm_readv);
    DENY_SYSCALL(SYS_process_vm_writev);
    DENY_SYSCALL(SYS_pidfd_getfd);
    DENY_SYSCALL(SYS_kcmp);
    DENY_SYSCALL(SYS_process_madvise);
    DENY_SYSCALL(SYS_perf_event_open);
    DENY_SYSCALL(SYS_userfaultfd);
    DENY_SYSCALL(SYS_pidfd_send_signal);
    DENY_SYSCALL(SYS_rt_sigqueueinfo);
    DENY_SYSCALL(SYS_rt_tgsigqueueinfo);
    DENY_SYSCALL(SYS_fork);
    DENY_SYSCALL(SYS_vfork);
    DENY_SYSCALL(SYS_execve);
    DENY_SYSCALL(SYS_execveat);

#undef DENY_SYSCALL

    // clone is allowed only with CLONE_THREAD, so pthread_create works and a new process cannot be made.
    {
        size_t header = hf_filter_jump(filter, &length, BPF_JMP | BPF_JEQ | BPF_K, SYS_clone);
        filter[header].jt = 0;
        hf_filter_stmt(filter, &length, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0]));
        size_t thread_jump = hf_filter_jump(filter, &length, BPF_JMP | BPF_JSET | BPF_K, CLONE_THREAD);
        hf_filter_stmt(filter, &length, BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
        size_t allow = length;
        hf_filter_stmt(filter, &length, BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
        filter[header].jf = (unsigned char)(length - header - 1);
        filter_jump_to(filter, thread_jump, allow, true);
    }

    // kill(pid, ...) is allowed only for this process (or pid 0, which is this PID namespace's group).
    {
        size_t header = hf_filter_jump(filter, &length, BPF_JMP | BPF_JEQ | BPF_K, SYS_kill);
        filter[header].jt = 0;
        hf_filter_stmt(filter, &length, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0]));
        size_t zero_jump = hf_filter_jump(filter, &length, BPF_JMP | BPF_JEQ | BPF_K, 0);
        size_t self_jump = hf_filter_jump(filter, &length, BPF_JMP | BPF_JEQ | BPF_K, (unsigned int)self_pid);
        hf_filter_stmt(filter, &length, BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
        size_t allow = length;
        hf_filter_stmt(filter, &length, BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
        filter[header].jf = (unsigned char)(length - header - 1);
        filter_jump_to(filter, zero_jump, allow, true);
        filter_jump_to(filter, self_jump, allow, true);
    }

    // tkill(tid, ...) is allowed only for the main thread.
    {
        size_t header = hf_filter_jump(filter, &length, BPF_JMP | BPF_JEQ | BPF_K, SYS_tkill);
        filter[header].jt = 0;
        hf_filter_stmt(filter, &length, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0]));
        size_t self_jump = hf_filter_jump(filter, &length, BPF_JMP | BPF_JEQ | BPF_K, (unsigned int)self_tid);
        hf_filter_stmt(filter, &length, BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
        size_t allow = length;
        hf_filter_stmt(filter, &length, BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
        filter[header].jf = (unsigned char)(length - header - 1);
        filter_jump_to(filter, self_jump, allow, true);
    }

    // tgkill(tgid, tid, ...) is allowed only for this main thread.
    {
        size_t header = hf_filter_jump(filter, &length, BPF_JMP | BPF_JEQ | BPF_K, SYS_tgkill);
        filter[header].jt = 0;
        hf_filter_stmt(filter, &length, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0]));
        size_t group_jump = hf_filter_jump(filter, &length, BPF_JMP | BPF_JEQ | BPF_K, (unsigned int)self_pid);
        hf_filter_stmt(filter, &length, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[1]));
        size_t thread_jump = hf_filter_jump(filter, &length, BPF_JMP | BPF_JEQ | BPF_K, (unsigned int)self_tid);
        size_t deny = length;
        hf_filter_stmt(filter, &length, BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
        size_t allow = length;
        hf_filter_stmt(filter, &length, BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
        filter[header].jf = (unsigned char)(length - header - 1);
        filter[group_jump].jt = 0;
        filter[group_jump].jf = (unsigned char)(deny - group_jump - 1);
        filter_jump_to(filter, thread_jump, allow, true);
    }

    if (hf_filter_end(filter, length, MAX_FILTER) != 0) return -1;
    return 0;
}

static int write_attempt_denied(const char *path, int flags)
{
    int fd = open(path, flags | O_CLOEXEC, 0600);
    if (fd < 0) return errno;
    int result = write_all(fd, "canary", 6) == 0 ? 0 : errno;
    close(fd);
    return result;
}

static int canary_g1(struct shared_result *shared)
{
    char *paths = malloc(5 * PATH_MAX);
    if (paths == NULL)
    {
        set_result(shared, ITEM_G1, STATUS_FAIL, "could not allocate the G1 canary paths");
        return -1;
    }
    char *renamed = paths;
    char *directory = renamed + PATH_MAX;
    char *hardlink = directory + PATH_MAX;
    char *symlink_path = hardlink + PATH_MAX;
    char *landlock_file = symlink_path + PATH_MAX;
    joinf(renamed, PATH_MAX, protected_dir, "renamed");
    joinf(directory, PATH_MAX, protected_dir, "new-directory");
    joinf(hardlink, PATH_MAX, protected_dir, "hardlink");
    joinf(symlink_path, PATH_MAX, protected_dir, "symlink");
    joinf(landlock_file, PATH_MAX, landlock_rw, "must-not-write");

    int result = -1;
    int create_error = write_attempt_denied(protected_canary, O_WRONLY | O_CREAT | O_TRUNC);
    int truncate_error = 0;
    int fd = open(protected_canary, O_WRONLY | O_CLOEXEC);
    if (fd >= 0)
    {
        if (ftruncate(fd, 0) != 0) truncate_error = errno;
        close(fd);
    }
    else truncate_error = errno;
    int rename_ok = rename(protected_canary, renamed) == 0;
    int unlink_ok = unlink(protected_canary) == 0;
    int mkdir_ok = mkdir(directory, 0700) == 0;
    int link_ok = link(protected_canary, hardlink) == 0;
    int symlink_ok = symlink("target", symlink_path) == 0;
    int landlock_error = write_attempt_denied(landlock_file, O_WRONLY | O_CREAT | O_TRUNC);

    if (create_error == 0 || truncate_error == 0 || rename_ok || unlink_ok || mkdir_ok || link_ok || symlink_ok)
    {
        set_result(shared, ITEM_G1, STATUS_FAIL,
            "an outside write succeeded (create=%d truncate=%d rename=%d unlink=%d mkdir=%d link=%d symlink=%d)",
            create_error, truncate_error, rename_ok, unlink_ok, mkdir_ok, link_ok, symlink_ok);
        goto done;
    }
    if (landlock_error == 0)
    {
        set_result(shared, ITEM_G1, STATUS_FAIL, "Landlock allowed a writable non-session tmpfs");
        goto done;
    }
    set_result(shared, ITEM_G1, STATUS_PASS,
        "outside create/truncate/rename/unlink/mkdir/link/symlink denied (%s); non-session tmpfs write denied (%s)",
        strerror(create_error), strerror(landlock_error));
    if (shared->status[ITEM_G1_LANDLOCK] == STATUS_PENDING)
        set_result(shared, ITEM_G1_LANDLOCK, STATUS_PASS, "a writable tmpfs outside the session folder refused a create: %s", strerror(landlock_error));
    result = 0;

done:
    free(paths);
    return result;
}

static int canary_g1_allowed(struct shared_result *shared)
{
    char file[PATH_MAX], directory[PATH_MAX];
    joinf(file, sizeof(file), session_dir, "allowed.txt");
    joinf(directory, sizeof(directory), session_dir, "allowed-directory");
    if (write_file(file, "allowed") != 0)
    {
        child_failure(shared, ITEM_G1_ALLOWED, errno, "writing a session file");
        return -1;
    }
    if (mkdir(directory, 0700) != 0)
    {
        child_failure(shared, ITEM_G1_ALLOWED, errno, "creating a session folder");
        return -1;
    }
    char text[32];
    if (read_text(file, text, sizeof(text)) != 0 || strcmp(text, "allowed") != 0)
    {
        set_result(shared, ITEM_G1_ALLOWED, STATUS_FAIL, "the session file did not read back unchanged");
        return -1;
    }
    set_result(shared, ITEM_G1_ALLOWED, STATUS_PASS, "wrote, read and made folders inside the session folder");
    return 0;
}

static int canary_g2(struct shared_result *shared)
{
    pid_t victim = shared->victim_pid;
    char sent[] = "attacker-write";
    struct iovec local = { .iov_base = sent, .iov_len = sizeof(sent) };
    struct iovec remote = { .iov_base = (void *)victim_buffer, .iov_len = sizeof(sent) };
    errno = 0;
    long vm = syscall(SYS_process_vm_writev, victim, &local, 1, &remote, 1, 0);
    int vm_error = errno;
    errno = 0;
    long traced = ptrace(PTRACE_ATTACH, victim, NULL, NULL);
    int ptrace_error = errno;
    if (traced == 0) ptrace(PTRACE_DETACH, victim, NULL, NULL);
    errno = 0;
    int signalled = kill(victim, SIGUSR1);
    int signal_error = errno;

    if (vm >= 0 || traced == 0 || signalled == 0)
    {
        set_result(shared, ITEM_G2, STATUS_FAIL,
            "an outside action succeeded (process_vm_writev=%ld ptrace=%ld kill=%d): %s/%s/%s",
            vm, traced, signalled, strerror(vm_error), strerror(ptrace_error), strerror(signal_error));
        return -1;
    }
    set_result(shared, ITEM_G2, STATUS_PASS, "outside process_vm_writev: %s; ptrace: %s; kill: %s",
        strerror(vm_error), strerror(ptrace_error), strerror(signal_error));
    return 0;
}

static int canary_g2_self_signal(struct shared_result *shared)
{
    self_signal_seen = 0;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = self_signal_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) != 0)
    {
        child_failure(shared, ITEM_G2_SELF_SIGNAL, errno, "sigaction");
        return -1;
    }
    if (kill(getpid(), SIGUSR1) != 0)
    {
        child_failure(shared, ITEM_G2_SELF_SIGNAL, errno, "a self signal was refused");
        return -1;
    }
    if (!self_signal_seen)
    {
        set_result(shared, ITEM_G2_SELF_SIGNAL, STATUS_FAIL, "the self signal was allowed but not delivered");
        return -1;
    }
    set_result(shared, ITEM_G2_SELF_SIGNAL, STATUS_PASS, "kill(getpid(), SIGUSR1) was delivered to the handler");
    return 0;
}

static void *canary_thread_main(void *argument)
{
    (void)argument;
    return NULL;
}

static int canary_g3_threads(struct shared_result *shared)
{
    pthread_t thread;
    int error = pthread_create(&thread, NULL, canary_thread_main, NULL);
    if (error != 0)
    {
        set_result(shared, ITEM_G3_THREADS, STATUS_FAIL, "pthread_create: %s", strerror(error));
        return -1;
    }
    error = pthread_join(thread, NULL);
    if (error != 0)
    {
        set_result(shared, ITEM_G3_THREADS, STATUS_FAIL, "pthread_join: %s", strerror(error));
        return -1;
    }
    set_result(shared, ITEM_G3_THREADS, STATUS_PASS, "pthread_create and pthread_join succeeded");
    return 0;
}

struct g3_failure
{
    const char *what;
    int error;
};

static bool g3_denied(long result, const char *spawned, const char *not_denied, bool allow_enosys,
    struct g3_failure *failure)
{
    if (result == 0) _exit(0);
    if (result > 0)
    {
        waitpid((pid_t)result, NULL, 0);
        failure->what = spawned;
        return false;
    }
    if (errno == EPERM || (allow_enosys && errno == ENOSYS)) return true;
    failure->what = not_denied;
    failure->error = errno;
    return false;
}

static int g3_report_failure(struct shared_result *shared, const struct g3_failure *failure)
{
    set_result(shared, ITEM_G3, STATUS_FAIL, "%s%s%s", failure->what,
        failure->error ? ": " : "", failure->error ? strerror(failure->error) : "");
    return -1;
}

static int g3_exec_denied(struct shared_result *shared)
{
    char *const arguments[] = { (char *)"/bin/true", NULL };
    char *const environment[] = { NULL };
    execve("/bin/true", arguments, environment);
    if (errno != EPERM)
    {
        set_result(shared, ITEM_G3, STATUS_FAIL, "execve was not denied with EPERM: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int canary_g3(struct shared_result *shared)
{
    struct g3_failure failure = { 0 };
    if (!g3_denied(fork(), "fork", "fork was not denied with EPERM", false, &failure))
        return g3_report_failure(shared, &failure);
    if (!g3_denied(vfork(), "vfork", "vfork was not denied with EPERM", false, &failure))
        return g3_report_failure(shared, &failure);
    if (!g3_denied(syscall(SYS_clone, SIGCHLD, 0, 0, 0, 0), "clone without CLONE_THREAD",
        "clone without CLONE_THREAD was not denied with EPERM", false, &failure))
        return g3_report_failure(shared, &failure);
    if (!g3_denied(syscall(SYS_clone, CLONE_NEWUSER | SIGCHLD, 0, 0, 0, 0), "clone with CLONE_NEWUSER",
        "clone with CLONE_NEWUSER was not denied with EPERM", false, &failure))
        return g3_report_failure(shared, &failure);
    if (!g3_denied(syscall(SYS_getpid | __X32_SYSCALL_BIT), "x32-numbered syscall",
        "an x32-numbered syscall was not denied with EPERM", false, &failure))
        return g3_report_failure(shared, &failure);

    struct clone_args arguments;
    memset(&arguments, 0, sizeof(arguments));
    arguments.exit_signal = SIGCHLD;
    errno = 0;
    long clone3_result = syscall(SYS_clone3, &arguments, sizeof(arguments));
    if (clone3_result != -1 || errno != ENOSYS)
    {
        set_result(shared, ITEM_G3, STATUS_FAIL, "clone3 was not denied with ENOSYS: %s", strerror(errno));
        return -1;
    }

    // This is last: if the filter were absent, a successful exec replaces this process before it can report.
    if (g3_exec_denied(shared) != 0) return -1;
    set_result(shared, ITEM_G3, STATUS_PASS,
        "fork, vfork, clone without CLONE_THREAD, clone(CLONE_NEWUSER), x32, clone3 (ENOSYS) and execve were denied");
    return 0;
}

static int canary_g4(struct shared_result *shared)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        child_failure(shared, ITEM_G4, errno, "socket");
        return -1;
    }
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)shared->listener_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int result = connect(fd, (struct sockaddr *)&address, sizeof(address));
    int saved = errno;
    close(fd);
    if (result == 0)
    {
        set_result(shared, ITEM_G4, STATUS_FAIL, "the outside loopback port %d was reachable from the sandbox",
            shared->listener_port);
        return -1;
    }
    set_result(shared, ITEM_G4, STATUS_PASS, "outside loopback port %d refused the connection: %s",
        shared->listener_port, strerror(saved));
    return 0;
}

static void close_inherited_fds(void)
{
    if (syscall(SYS_close_range, 3U, 1024U, 0) != 0)
        for (int fd = 3; fd <= 1024; fd++) close(fd);
}

static void fail_pending(struct shared_result *shared, int first, int last, const char *reason)
{
    for (int item = first; item <= last; item++)
        if (shared->status[item] == STATUS_PENDING)
            set_result(shared, item, STATUS_FAIL, "%s", reason);
}

static int run_canary_child(struct shared_result *shared, int first, int last, const char *name,
    int (*canary)(struct shared_result *))
{
    run_child(shared, first, CANARY_TIMEOUT_SECONDS, canary);
    char reason[160];
    snprintf(reason, sizeof(reason), "the %s canary child did not report", name);
    fail_pending(shared, first, last, reason);
    return 0;
}

static int g1_child(struct shared_result *shared)
{
    close_inherited_fds();
    int error = enter_user_namespace();
    if (error != 0) { child_failure(shared, ITEM_G1, -error, "user namespace for G1"); return 1; }
    if (syscall(SYS_unshare, CLONE_NEWNS) != 0) { child_failure(shared, ITEM_G1, errno, "CLONE_NEWNS for G1"); return 1; }
    error = private_mount_tree();
    if (error != 0) { child_failure(shared, ITEM_G1, -error, "making G1 mount propagation private"); return 1; }
    error = make_root_readonly(session_dir);
    if (error != 0) { child_failure(shared, ITEM_G1, -error, "building the G1 read-only root with its writable session bind"); return 1; }
    if (mount("tmpfs", landlock_rw, "tmpfs", MS_NOSUID | MS_NODEV, "mode=0700,size=16m") != 0)
    {
        child_failure(shared, ITEM_G1, errno, "mounting G1's writable non-session test folder");
        return 1;
    }
    if (apply_landlock(shared, ITEM_G1_LANDLOCK) != 0)
    {
        set_result(shared, ITEM_G1, STATUS_FAIL, "G1 cannot run because Landlock could not be applied");
        return 1;
    }
    canary_g1(shared);
    canary_g1_allowed(shared);
    return shared->status[ITEM_G1] == STATUS_PASS && shared->status[ITEM_G1_ALLOWED] == STATUS_PASS ? 0 : 1;
}

static int g2_child(struct shared_result *shared)
{
    close_inherited_fds();
    int error = enter_user_namespace();
    if (error != 0) { child_failure(shared, ITEM_G2, -error, "user namespace for G2"); return 1; }
    if (syscall(SYS_unshare, CLONE_NEWPID) != 0) { child_failure(shared, ITEM_G2, errno, "CLONE_NEWPID for G2"); return 1; }

    pid_t child = fork();
    if (child < 0) { child_failure(shared, ITEM_G2, errno, "forking the G2 canary"); return 1; }
    if (child == 0)
    {
        if (install_canary_seccomp(getpid(), (pid_t)syscall(SYS_gettid)) != 0)
        {
            int saved = errno;
            set_result(shared, ITEM_G2, STATUS_FAIL, "G2 seccomp could not be installed: %s (errno %d)", strerror(saved), saved);
            set_result(shared, ITEM_G2_SELF_SIGNAL, STATUS_FAIL, "G2 seccomp could not be installed: %s (errno %d)", strerror(saved), saved);
            _exit(1);
        }
        canary_g2(shared);
        canary_g2_self_signal(shared);
        _exit(0);
    }

    int status = 0;
    int waited = wait_bounded(child, CANARY_TIMEOUT_SECONDS - 5, &status);
    if (waited == 0)
    {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        fail_pending(shared, ITEM_G2, ITEM_G2_SELF_SIGNAL, "the G2 canary process did not finish in time");
        return 1;
    }
    if (waited < 0)
        fail_pending(shared, ITEM_G2, ITEM_G2_SELF_SIGNAL, "waiting for the G2 canary process failed");
    fail_pending(shared, ITEM_G2, ITEM_G2_SELF_SIGNAL, "the G2 canary process ended without reporting");
    return shared->status[ITEM_G2] == STATUS_PASS && shared->status[ITEM_G2_SELF_SIGNAL] == STATUS_PASS ? 0 : 1;
}

static int g3_child(struct shared_result *shared)
{
    close_inherited_fds();
    if (install_canary_seccomp(getpid(), (pid_t)syscall(SYS_gettid)) != 0)
    {
        int saved = errno;
        set_result(shared, ITEM_G3, STATUS_FAIL, "G3 seccomp could not be installed: %s (errno %d)", strerror(saved), saved);
        set_result(shared, ITEM_G3_THREADS, STATUS_FAIL, "G3 seccomp could not be installed: %s (errno %d)", strerror(saved), saved);
        return 1;
    }
    canary_g3_threads(shared);
    canary_g3(shared);
    return 0;
}

static int g4_child(struct shared_result *shared)
{
    close_inherited_fds();
    int error = enter_user_namespace();
    if (error != 0) { child_failure(shared, ITEM_G4, -error, "user namespace for G4"); return 1; }
    if (syscall(SYS_unshare, CLONE_NEWNET) != 0) { child_failure(shared, ITEM_G4, errno, "CLONE_NEWNET for G4"); return 1; }
    if (loopback_up() != 0) { child_failure(shared, ITEM_G4, errno, "bringing up loopback in G4's network namespace"); return 1; }
    canary_g4(shared);
    return shared->status[ITEM_G4] == STATUS_PASS ? 0 : 1;
}

// Each canary runs in its own throwaway child. Only G1 creates the read-only filesystem view; G2 needs a user and PID
// namespace, G3 needs seccomp alone, and G4 needs a user and network namespace. The parent never enters a namespace.
int run_canary(struct shared_result *shared)
{
    run_canary_child(shared, ITEM_G1, ITEM_G1_ALLOWED, "G1", g1_child);
    run_canary_child(shared, ITEM_G2, ITEM_G2_SELF_SIGNAL, "G2", g2_child);
    run_canary_child(shared, ITEM_G3, ITEM_G3_THREADS, "G3", g3_child);
    run_canary_child(shared, ITEM_G4, ITEM_G4, "G4", g4_child);
    return 0;
}

static int start_victim(int stop_read_fd, int stop_write_fd)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0)
    {
        close(stop_write_fd);
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = self_signal_handler;
        sigemptyset(&action.sa_mask);
        sigaction(SIGUSR1, &action, NULL);
        unsigned char byte;
        while (read(stop_read_fd, &byte, 1) < 0 && errno == EINTR) { }
        _exit(0);
    }
    return (int)pid;
}

static int start_listener(struct shared_result *shared)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 4) != 0)
    {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    socklen_t length = sizeof(address);
    if (getsockname(fd, (struct sockaddr *)&address, &length) != 0)
    {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    shared->listener_port = ntohs(address.sin_port);
    return fd;
}

static void json_string(FILE *file, const char *text)
{
    fputc('"', file);
    for (const unsigned char *at = (const unsigned char *)text; *at != '\0'; at++)
    {
        switch (*at)
        {
            case '"': fputs("\\\"", file); break;
            case '\\': fputs("\\\\", file); break;
            case '\b': fputs("\\b", file); break;
            case '\f': fputs("\\f", file); break;
            case '\n': fputs("\\n", file); break;
            case '\r': fputs("\\r", file); break;
            case '\t': fputs("\\t", file); break;
            default:
                if (*at < 0x20) fprintf(file, "\\u%04x", *at);
                else fputc(*at, file);
                break;
        }
    }
    fputc('"', file);
}

static bool report_is_ok(struct shared_result *shared)
{
    for (int item = 0; item < ITEM_COUNT; item++)
        if (shared->status[item] != STATUS_PASS) return false;
    return true;
}

static int first_reason(struct shared_result *shared, char *out, size_t size)
{
    for (int item = 0; item < ITEM_COUNT; item++)
    {
        if (shared->status[item] == STATUS_PASS) continue;
        snprintf(out, size, "%s: %s", ITEMS[item].title, shared->evidence[item]);
        return item;
    }
    snprintf(out, size, "all items passed");
    return -1;
}

static int write_report(const char *path, struct shared_result *shared, const char *first)
{
    if (make_parent_dirs(path) != 0) return -1;
    char temporary[PATH_MAX];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int)sizeof(temporary))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    FILE *file = fopen(temporary, "w");
    if (file == NULL) return -1;
    char timestamp[64];
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    struct utsname system;
    if (uname(&system) != 0)
    {
        memset(&system, 0, sizeof(system));
        strcpy(system.sysname, "unknown");
        strcpy(system.release, "unknown");
        strcpy(system.machine, "unknown");
    }

    fprintf(file, "{\n  \"schema\": 1,\n  \"tool\": \"hf-sandbox-probe\",\n  \"version\": \"F.4a\",\n");
    fprintf(file, "  \"generatedUtc\": ");
    json_string(file, timestamp);
    fprintf(file, ",\n  \"kernel\": ");
    json_string(file, system.sysname);
    fprintf(file, ",\n  \"kernelRelease\": ");
    json_string(file, system.release);
    fprintf(file, ",\n  \"machine\": ");
    json_string(file, system.machine);
    fprintf(file, ",\n  \"uid\": %u,\n  \"ok\": %s,\n  \"firstReason\": ", (unsigned)host_uid, report_is_ok(shared) ? "true" : "false");
    json_string(file, first);
    fprintf(file, ",\n  \"items\": [\n");
    for (int item = 0; item < ITEM_COUNT; item++)
    {
        fprintf(file, "    { \"id\": ");
        json_string(file, ITEMS[item].id);
        fprintf(file, ", \"group\": ");
        json_string(file, ITEMS[item].group);
        fprintf(file, ", \"title\": ");
        json_string(file, ITEMS[item].title);
        fprintf(file, ", \"status\": ");
        json_string(file, status_name(shared->status[item]));
        fprintf(file, ", \"evidence\": ");
        json_string(file, shared->evidence[item]);
        fprintf(file, " }%s\n", item + 1 == ITEM_COUNT ? "" : ",");
    }
    fprintf(file, "  ]\n}\n");
    if (fclose(file) != 0)
    {
        unlink(temporary);
        return -1;
    }
    if (rename(temporary, path) != 0)
    {
        int saved = errno;
        unlink(temporary);
        errno = saved;
        return -1;
    }
    return 0;
}

static void print_items(struct shared_result *shared)
{
    for (int item = 0; item < ITEM_COUNT; item++)
        printf("%-4s %-9s %s: %s\n", status_name(shared->status[item]), ITEMS[item].group, ITEMS[item].title,
            shared->evidence[item]);
}

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s [--json <file>]\n", program);
}

// 0: run, 1: help printed, 2: bad arguments.
static int parse_arguments(int argc, char **argv, const char **json)
{
    *json = "hf-sandbox-probe.json";
    for (int index = 1; index < argc; index++)
    {
        if (strcmp(argv[index], "--json") == 0 && index + 1 < argc)
        {
            *json = argv[++index];
        }
        else if (strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0)
        {
            usage(argv[0]);
            return 1;
        }
        else
        {
            usage(argv[0]);
            return 2;
        }
    }
    return 0;
}

static int prepare_paths(void)
{
    host_uid = getuid();
    host_gid = getgid();
    const char *temporary_base = getenv("TMPDIR");
    if (temporary_base == NULL || temporary_base[0] == '\0') temporary_base = "/tmp";
    if (snprintf(temp_root, sizeof(temp_root), "%s/hf-sandbox-probe.XXXXXX", temporary_base) >= (int)sizeof(temp_root))
    {
        fprintf(stderr, "hf-sandbox-probe: FAIL: the temporary path is too long\n");
        return -1;
    }
    if (mkdtemp(temp_root) == NULL)
    {
        fprintf(stderr, "hf-sandbox-probe: FAIL: could not make a temporary folder: %s\n", strerror(errno));
        return -1;
    }
    joinf(protected_dir, sizeof(protected_dir), temp_root, "protected");
    joinf(protected_canary, sizeof(protected_canary), protected_dir, "canary.txt");
    joinf(session_dir, sizeof(session_dir), temp_root, "session");
    joinf(landlock_rw, sizeof(landlock_rw), temp_root, "landlock-rw");
    if (make_dir(protected_dir) != 0 || make_dir(session_dir) != 0 || make_dir(landlock_rw) != 0 ||
        write_file(protected_canary, "protected-original") != 0)
    {
        fprintf(stderr, "hf-sandbox-probe: FAIL: could not prepare temporary folders: %s\n", strerror(errno));
        remove_tree(temp_root);
        return -1;
    }
    return 0;
}

static void run_mechanisms(struct shared_result *shared, bool have_tmp_canary, bool have_shm_canary)
{
    probe_landlock(shared);
    run_probe_child(shared, ITEM_SECCOMP_FILTER, probe_seccomp_filter_child);
    probe_seccomp_user_notification(shared);
    run_probe_child(shared, ITEM_USERNS, probe_userns_child);
    run_probe_child(shared, ITEM_MOUNTNS, probe_mountns_child);
    run_probe_child(shared, ITEM_PIDNS, probe_pidns_child);
    run_probe_child(shared, ITEM_IPCNS, probe_ipcns_child);
    run_probe_child(shared, ITEM_NETNS, probe_netns_child);
    run_probe_child(shared, ITEM_OVERLAYFS, probe_overlayfs_child);
    run_probe_child(shared, ITEM_READONLY_ROOT, probe_readonly_root_child);
    if (have_tmp_canary)
        run_probe_child(shared, ITEM_PRIVATE_TMP, probe_private_tmp_child);
    else
        set_result(shared, ITEM_PRIVATE_TMP, STATUS_SKIP, "could not create a /tmp canary file");
    if (have_shm_canary)
        run_probe_child(shared, ITEM_PRIVATE_SHM, probe_private_shm_child);
    else
        set_result(shared, ITEM_PRIVATE_SHM, STATUS_SKIP, "/dev/shm is missing or not writable");
    probe_openat2(shared);
    probe_close_range(shared);
    run_probe_child(shared, ITEM_NO_NEW_PRIVS, probe_no_new_privs_child);
    probe_apparmor(shared);

    // The four canaries, in one child that applies the mechanisms the design will use.
    run_canary(shared);

    // Do not trust a canary that changed the victim page or the protected file.
    char protected_text[64];
    if (read_text(protected_canary, protected_text, sizeof(protected_text)) != 0 ||
        strcmp(protected_text, "protected-original") != 0)
        set_result(shared, ITEM_G1, STATUS_FAIL, "the protected canary file changed");
    if (memcmp((const void *)victim_buffer, "victim-original", sizeof("victim-original")) != 0)
        set_result(shared, ITEM_G2, STATUS_FAIL, "the outside victim page changed");

    // The private-/tmp and /dev/shm children did not touch the host files; prove it from outside.
    if (have_tmp_canary)
    {
        char text[64];
        if (read_text(tmp_canary, text, sizeof(text)) != 0 || strcmp(text, "host-tmp") != 0)
            set_result(shared, ITEM_PRIVATE_TMP, STATUS_FAIL, "the host /tmp canary changed");
    }
    if (have_shm_canary)
    {
        char text[64];
        if (read_text(shm_canary, text, sizeof(text)) != 0 || strcmp(text, "host-shm") != 0)
            set_result(shared, ITEM_PRIVATE_SHM, STATUS_FAIL, "the host /dev/shm canary changed");
    }
}

static bool write_summary(const char *json, struct shared_result *shared)
{
    char first[MAX_EVIDENCE + 160];
    first_reason(shared, first, sizeof(first));
    bool ok = report_is_ok(shared);
    print_items(shared);
    fflush(stdout);
    if (write_report(json, shared, first) != 0)
    {
        int saved = errno;
        fprintf(stderr, "hf-sandbox-probe: could not write the JSON report %s: %s\n", json, strerror(saved));
        if (ok)
        {
            ok = false;
            snprintf(first, sizeof(first), "could not write the JSON report %s: %s", json, strerror(saved));
        }
    }
    if (ok) printf("hf-sandbox-probe: PASS\n");
    else printf("hf-sandbox-probe: FAIL: %s\n", first);
    fflush(stdout);
    fflush(stderr);
    return ok;
}

struct probe_harness
{
    int stop_write;
    int listener;
    int victim;
};

static bool start_probe_harness(struct shared_result *shared, struct probe_harness *harness)
{
    harness->stop_write = -1;
    harness->listener = -1;
    harness->victim = 0;

    int stop_pipe[2];
    if (pipe(stop_pipe) != 0)
    {
        fprintf(stderr, "hf-sandbox-probe: FAIL: could not make the victim's stop pipe\n");
        return false;
    }
    int victim = start_victim(stop_pipe[0], stop_pipe[1]);
    close(stop_pipe[0]);
    if (victim <= 0)
    {
        fprintf(stderr, "hf-sandbox-probe: FAIL: could not start the process-isolation canary victim\n");
        close(stop_pipe[1]);
        return false;
    }
    shared->victim_pid = victim;

    int listener = start_listener(shared);
    if (listener < 0)
    {
        fprintf(stderr, "hf-sandbox-probe: FAIL: could not start the outside loopback listener\n");
        close(stop_pipe[1]);
        kill(victim, SIGTERM);
        waitpid(victim, NULL, 0);
        return false;
    }
    harness->stop_write = stop_pipe[1];
    harness->listener = listener;
    harness->victim = victim;
    return true;
}

static void finish_probe_harness(const struct probe_harness *harness, bool have_tmp, bool have_shm)
{
    // Cleanup is parent-side: the children's mount namespaces disappeared with them.
    close(harness->listener);
    unsigned char stop = 1;
    write_all(harness->stop_write, &stop, 1);
    close(harness->stop_write);
    int victim_status = 0;
    if (wait_bounded(harness->victim, 3, &victim_status) == 0)
    {
        kill(harness->victim, SIGTERM);
        waitpid(harness->victim, &victim_status, 0);
    }
    if (have_tmp) unlink(tmp_canary);
    if (have_shm) unlink(shm_canary);
    remove_tree(temp_root);
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    const char *json = NULL;
    int parsed = parse_arguments(argc, argv, &json);
    if (parsed != 0) return parsed == 1 ? 0 : 2;
    if (prepare_paths() != 0) return 1;

    struct shared_result *shared = NULL;
    if (!make_shared_result(&shared))
    {
        remove_tree(temp_root);
        return 1;
    }
    if (!make_victim_page())
    {
        munmap(shared, sizeof(*shared));
        remove_tree(temp_root);
        return 1;
    }

    struct probe_harness harness;
    if (!start_probe_harness(shared, &harness))
    {
        munmap((void *)victim_buffer, 4096);
        munmap(shared, sizeof(*shared));
        remove_tree(temp_root);
        return 1;
    }

    bool have_tmp_canary;
    bool have_shm_canary;
    make_host_canaries(&have_tmp_canary, &have_shm_canary);

    run_mechanisms(shared, have_tmp_canary, have_shm_canary);
    bool ok = write_summary(json, shared);

    finish_probe_harness(&harness, have_tmp_canary, have_shm_canary);
    munmap((void *)victim_buffer, 4096);
    munmap(shared, sizeof(*shared));
    return ok ? 0 : 1;
}
