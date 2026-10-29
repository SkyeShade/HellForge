// hf-sandbox --self-test: builds the same root view as a real session, applies the same guard, and runs the G1 canaries
// through it. Unlike the phase-1 probe this is not a toy view: the overlay, the read-only binds, the private /tmp and
// /dev/shm and the session's one writable folder are the real ones. The installation is built in the default layout
// (inside the real prefix at pfx/drive_c/users/steamuser/Documents/HellForge), so every expected absence exists on the
// host first and the overlay mask is exercised through both Z: and C:. Any unexpected success is a failure.

#include "hf-sandbox.h"

#include <stdarg.h>

#define HOST_INSTALL "prefix/pfx/drive_c/users/steamuser/Documents/HellForge"

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

static void run_canaries(const struct hf_sandbox *sandbox)
{
    char path[PATH_MAX];
    if (hf_join(path, sizeof(path), sandbox->game_out, "allowed.txt") == 0)
        expect_allowed("session write", path, "allowed");
    if (hf_join(path, sizeof(path), sandbox->game_out, "allowed-dir") == 0 && mkdir(path, 0700) == 0)
        report("session mkdir", true, "%s was created", path);
    else report("session mkdir", false, "%s: %s", path, strerror(errno));

    check_denied_paths(sandbox, sandbox->overlay_install);
    check_game_copy_operations(sandbox);
    check_overlay_game_copy(sandbox);

    if (hf_join(path, sizeof(path), sandbox->merged, "pfx/drive_c/overlay-write") == 0)
        expect_allowed("overlay write", path, "overlay");

    check_private_mount("private /tmp", sandbox->selftest_tmp_canary, "/tmp/hf-sandbox-selftest-private");
    check_private_mount("private /dev/shm", sandbox->selftest_shm_canary, "/dev/shm/hf-sandbox-selftest-private");
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

static int prepare_tree(const char *directory, char *temporary, size_t size, char *host_tmp, size_t host_tmp_size,
    char *host_shm, size_t host_shm_size)
{
    char base[PATH_MAX];
    if (self_test_base(directory, base, sizeof(base)) != 0) return 1;
    if (snprintf(temporary, size, "%s/.hf-sandbox-selftest.XXXXXX", base) >= (int)size)
        return hf_fail_msg("the self-test temporary path is too long");
    if (mkdtemp(temporary) == NULL) return hf_fail("creating the self-test temporary folder");
    const char *dirs[] =
    {
        HOST_INSTALL "/game/data", HOST_INSTALL "/profiles/default", HOST_INSTALL "/mods",
        HOST_INSTALL "/userdata", HOST_INSTALL "/account-snapshot", "outside",
    };
    for (size_t index = 0; index < sizeof(dirs) / sizeof(dirs[0]); index++)
        if (make_tree_dir(temporary, dirs[index]) != 0) return 1;
    const struct { const char *name; const char *text; } files[] =
    {
        { HOST_INSTALL "/game/data/keep.txt", "game-copy" },
        { HOST_INSTALL "/hellforge-install.json", "{}" },
        { HOST_INSTALL "/launcher-settings.json", "{\"ui_scale\":1}" },
        { HOST_INSTALL "/profiles/default/settings.ini", "profile" },
        { HOST_INSTALL "/mods/mod.txt", "mod" },
        { HOST_INSTALL "/userdata/save.dat", "save" },
        { HOST_INSTALL "/account-snapshot/seal.key", "seal" },
        { "prefix/pfx/system.reg", "REAL-PREFIX" },
        { "outside/protected.txt", "outside-protected" },
    };
    for (size_t index = 0; index < sizeof(files) / sizeof(files[0]); index++)
        if (write_tree_file(temporary, files[index].name, files[index].text) != 0) return 1;
    snprintf(host_tmp, host_tmp_size, "/tmp/hf-sandbox-selftest-host-%ld", (long)getpid());
    snprintf(host_shm, host_shm_size, "/dev/shm/hf-sandbox-selftest-host-%ld", (long)getpid());
    // The host canaries make the private-mount checks meaningful. A host without /dev/shm can still run the self-test;
    // its private /dev/shm tmpfs is made by the view regardless.
    if (write_file(host_tmp, "host-tmp") != 0) host_tmp[0] = '\0';
    if (write_file(host_shm, "host-shm") != 0) host_shm[0] = '\0';
    return 0;
}

// Every expected denial must exist on the host before the view is built. This is the parent's check, before the fork.
static void check_host_tree(const char *temporary)
{
    static const char *const files[] =
    {
        HOST_INSTALL "/profiles/default/settings.ini", HOST_INSTALL "/mods/mod.txt",
        HOST_INSTALL "/userdata/save.dat", HOST_INSTALL "/account-snapshot/seal.key",
        HOST_INSTALL "/launcher-settings.json", HOST_INSTALL "/game/data/keep.txt",
        HOST_INSTALL "/hellforge-install.json",
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

static int verify_after(const char *temporary, const struct hf_sandbox *sandbox)
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
    if (passed) printf("PASS overlay: lower unchanged; the upper layer and the session folder hold the writes\n");
    return passed ? 0 : 1;
}

static void clean_up(const char *temporary, const char *host_tmp, const char *host_shm)
{
    remove_tree(temporary);
    unlink(host_tmp);
    unlink(host_shm);
}

static bool enter_selftest_view(struct hf_sandbox *sandbox)
{
    if (hf_enter_namespaces(sandbox) != 0 || hf_mount_overlay(sandbox) != 0 ||
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
    char temporary[PATH_MAX] = "", host_tmp[PATH_MAX] = "", host_shm[PATH_MAX] = "";
    if (prepare_tree(directory, temporary, sizeof(temporary), host_tmp, sizeof(host_tmp), host_shm, sizeof(host_shm)) != 0)
    {
        clean_up(temporary, host_tmp, host_shm);
        return 1;
    }
    struct hf_sandbox sandbox;
    memset(&sandbox, 0, sizeof(sandbox));
    snprintf(sandbox.selftest_tmp_canary, sizeof(sandbox.selftest_tmp_canary), "%s", host_tmp);
    snprintf(sandbox.selftest_shm_canary, sizeof(sandbox.selftest_shm_canary), "%s", host_shm);
    if (hf_prepare_selftest_paths(&sandbox, temporary) != 0 || hf_create_session_dirs(&sandbox) != 0 ||
        hf_prepare_session_home(&sandbox) != 0)
    {
        clean_up(temporary, host_tmp, host_shm);
        return 1;
    }
    failures = 0;
    check_host_tree(temporary);
    if (failures != 0)
    {
        clean_up(temporary, host_tmp, host_shm);
        return 1;
    }

    pid_t child = start_selftest_child(&sandbox);
    if (child < 0)
    {
        hf_fail("forking the self-test");
        clean_up(temporary, host_tmp, host_shm);
        return 1;
    }
    int status = 0;
    bool finished;
    wait_selftest_child(child, &status, &finished);
    int after = verify_after(temporary, &sandbox);
    clean_up(temporary, host_tmp, host_shm);
    return report_selftest(finished, status, after) ? 0 : 1;
}
