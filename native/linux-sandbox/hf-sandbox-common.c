// The small filesystem, namespace and process helpers shared by hf-sandbox-probe and hf-sandbox. See hf-sandbox-common.h.

#include "hf-sandbox-common.h"

int hf_join(char *out, size_t size, const char *base, const char *name)
{
    int written = snprintf(out, size, "%s/%s", base, name);
    if (written < 0 || (size_t)written >= size)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int hf_join3(char *out, size_t size, const char *first, const char *second, const char *third)
{
    int written = snprintf(out, size, "%s/%s/%s", first, second, third);
    if (written < 0 || (size_t)written >= size)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

void joinf(char *out, size_t size, const char *base, const char *name)
{
    snprintf(out, size, "%s/%s", base, name);
}

int make_dir(const char *path)
{
    if (mkdir(path, 0700) == 0 || errno == EEXIST) return 0;
    return -1;
}

int make_parent_dirs(const char *path)
{
    char copy[PATH_MAX];
    if (strlen(path) >= sizeof(copy)) { errno = ENAMETOOLONG; return -1; }
    strcpy(copy, path);
    char *slash = strrchr(copy, '/');
    if (slash == NULL || slash == copy) return 0;
    *slash = '\0';
    for (char *at = copy + 1; *at != '\0'; at++)
    {
        if (*at != '/') continue;
        *at = '\0';
        if (mkdir(copy, 0755) != 0 && errno != EEXIST) return -1;
        *at = '/';
    }
    if (mkdir(copy, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int write_all(int fd, const void *data, size_t length)
{
    const unsigned char *at = data;
    while (length > 0)
    {
        ssize_t written = write(fd, at, length);
        if (written < 0)
        {
            if (errno == EINTR) continue;
            return -1;
        }
        at += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

static int write_proc(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int result = write_all(fd, text, strlen(text));
    int saved = errno;
    close(fd);
    errno = saved;
    return result;
}

int write_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int result = write_all(fd, text, strlen(text));
    int saved = errno;
    close(fd);
    errno = saved;
    return result;
}

int read_text(const char *path, char *out, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t got = read(fd, out, size - 1);
    int saved = errno;
    close(fd);
    if (got < 0) { errno = saved; return -1; }
    out[got] = '\0';
    return 0;
}

int path_exists(const char *path)
{
    struct stat info;
    return lstat(path, &info) == 0;
}

void remove_tree(const char *path)
{
    struct stat info;
    if (lstat(path, &info) != 0) return;
    if (!S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode))
    {
        unlink(path);
        return;
    }
    // The probe's overlay check leaves its lower folder read-only (0500) and overlayfs leaves its work folder with no
    // permissions; both are ours, so give them back to the owner before emptying them.
    chmod(path, 0700);
    DIR *directory = opendir(path);
    if (directory != NULL)
    {
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL)
        {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            char child[PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
            remove_tree(child);
        }
        closedir(directory);
    }
    rmdir(path);
}

int enter_user_namespace(void)
{
    // Capture the host ids before unshare: afterwards this process has no mapping yet and getuid() reports nobody.
    uid_t uid = getuid();
    gid_t gid = getgid();
    if (syscall(SYS_unshare, CLONE_NEWUSER) != 0) return -errno;

    // A single-threaded unprivileged process may map itself. No setuid helper, subuid or root is involved.
    if (write_proc("/proc/self/setgroups", "deny\n") != 0 && errno != ENOENT) return -errno;

    char map[96];
    snprintf(map, sizeof(map), "0 %u 1\n", (unsigned)uid);
    if (write_proc("/proc/self/uid_map", map) != 0) return -errno;
    snprintf(map, sizeof(map), "0 %u 1\n", (unsigned)gid);
    if (write_proc("/proc/self/gid_map", map) != 0) return -errno;
    if (getuid() != 0) return -EINVAL;
    return 0;
}

int private_mount_tree(void)
{
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) return -errno;
    return 0;
}

int wait_bounded(pid_t pid, int seconds, int *status_out)
{
    for (int tick = 0; tick < seconds * 100; tick++)
    {
        pid_t got = waitpid(pid, status_out, WNOHANG);
        if (got == pid) return 1;
        if (got < 0 && errno != EINTR) return -1;
        struct timespec pause = { 0, 10000000L };
        nanosleep(&pause, NULL);
    }
    return 0;
}

void hf_filter_stmt(struct sock_filter *filter, size_t *length, unsigned short code, unsigned int value)
{
    filter[*length].code = code;
    filter[*length].jt = 0;
    filter[*length].jf = 0;
    filter[*length].k = value;
    (*length)++;
}

size_t hf_filter_jump(struct sock_filter *filter, size_t *length, unsigned short code, unsigned int value)
{
    size_t at = *length;
    hf_filter_stmt(filter, length, code, value);
    return at;
}

void hf_filter_deny(struct sock_filter *filter, size_t *length, unsigned int number)
{
    hf_filter_deny_errno(filter, length, number, EPERM);
}

void hf_filter_deny_errno(struct sock_filter *filter, size_t *length, unsigned int number, unsigned int error)
{
    size_t jump = hf_filter_jump(filter, length, BPF_JMP | BPF_JEQ | BPF_K, number);
    filter[jump].jt = 0;
    filter[jump].jf = 1;
    hf_filter_stmt(filter, length, BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (error & SECCOMP_RET_DATA));
}

size_t hf_filter_begin(struct sock_filter *filter, size_t capacity)
{
    size_t length = 0;
    hf_filter_stmt(filter, &length, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
    size_t arch_jump = hf_filter_jump(filter, &length, BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64);
    filter[arch_jump].jt = 1;
    filter[arch_jump].jf = 0;
    hf_filter_stmt(filter, &length, BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    hf_filter_stmt(filter, &length, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));

    // x32 numbers carry bit 30. The number list is the x86_64 ABI; deny the whole x32 range before comparing numbers.
    size_t x32_jump = hf_filter_jump(filter, &length, BPF_JMP | BPF_JSET | BPF_K, __X32_SYSCALL_BIT);
    filter[x32_jump].jt = 0;
    filter[x32_jump].jf = 1;
    hf_filter_stmt(filter, &length, BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));

    // The mount family, the namespace switches and the kernel doors that could build or inspect another view. Both
    // programs deny these; the probe adds the phase-3 process rules and the sandbox adds its own.
    static const unsigned int denied[] =
    {
        SYS_mount, SYS_umount2, SYS_pivot_root, SYS_chroot, SYS_setns, SYS_unshare, SYS_open_tree, SYS_move_mount,
        SYS_fsopen, SYS_fsmount, SYS_fspick, SYS_mount_setattr, SYS_open_by_handle_at, SYS_name_to_handle_at, SYS_bpf,
        SYS_io_uring_setup, SYS_add_key, SYS_request_key, SYS_keyctl,
    };
    for (size_t index = 0; index < sizeof(denied) / sizeof(denied[0]); index++)
        hf_filter_deny(filter, &length, denied[index]);
    // clone3 carries its flags in memory; return ENOSYS so glibc, musl and Wine fall back to clone, whose namespace bits
    // are checked by hf_filter_deny_clone_namespaces. The probe accepts ENOSYS in its G3 canary.
    hf_filter_deny_errno(filter, &length, SYS_clone3, ENOSYS);
    if (length > capacity) errno = E2BIG;
    return length;
}

int hf_filter_deny_clone_namespaces(struct sock_filter *filter, size_t *length, size_t capacity)
{
    if (*length + 6 > capacity) { errno = E2BIG; return -1; }
    // LD nr; JEQ clone -> LD args[0]; JSET CLONE_NEW* -> RET EPERM; LD nr. The last LD restores A for the caller's next
    // rule (the probe's clone thread rule and kill rules, or the sandbox's final ALLOW).
    hf_filter_stmt(filter, length, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
    size_t header = hf_filter_jump(filter, length, BPF_JMP | BPF_JEQ | BPF_K, SYS_clone);
    hf_filter_stmt(filter, length, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0]));
    size_t flags = hf_filter_jump(filter, length, BPF_JMP | BPF_JSET | BPF_K, HF_CLONE_NEW_MASK);
    hf_filter_stmt(filter, length, BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA));
    hf_filter_stmt(filter, length, BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
    filter[header].jt = 0;
    filter[header].jf = 4; // non-clone: from the JEQ to the instruction after the six-instruction block
    filter[flags].jt = 0;  // CLONE_NEW* set: next is RET EPERM
    filter[flags].jf = 1;  // no namespace bit: run the restoring LD nr, then the caller's next instruction
    return 0;
}

int hf_filter_end(struct sock_filter *filter, size_t length, size_t capacity)
{
    hf_filter_stmt(filter, &length, BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    if (length > capacity) { errno = E2BIG; return -1; }
    struct sock_fprog program = { .len = (unsigned short)length, .filter = filter };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return -1;
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) return -1;
    return 0;
}
