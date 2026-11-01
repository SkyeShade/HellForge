// Landlock, seccomp, the capability drop and the before-exec proof. These are kernel boundaries: they apply to native
// code calling open/truncate/link/mount directly, not only to Win32. See review-F1.md 4.2 and 5.1.

#include "hf-sandbox.h"

#include <linux/audit.h>
#include <linux/capability.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/resource.h>

#ifndef SECBIT_NOROOT
#define SECBIT_NOROOT (1 << 0)
#define SECBIT_NOROOT_LOCKED (1 << 1)
#define SECBIT_NO_SETUID_FIXUP (1 << 2)
#define SECBIT_NO_SETUID_FIXUP_LOCKED (1 << 3)
#endif
#ifndef SECCOMP_RET_USER_NOTIF
#define SECCOMP_RET_USER_NOTIF 0x7fc00000U
#endif
#ifndef PR_CAP_AMBIENT
#define PR_CAP_AMBIENT 47
#define PR_CAP_AMBIENT_IS_SET 1
#define PR_CAP_AMBIENT_CLEAR_ALL 4
#endif

static bool path_is_ancestor(const char *ancestor, const char *path)
{
    size_t length = strlen(ancestor);
    while (length > 1 && ancestor[length - 1] == '/') length--;
    if (length == 1 && ancestor[0] == '/') return path[0] == '/';
    if (length == 0 || strncmp(ancestor, path, length) != 0) return false;
    return path[length] == '\0' || path[length] == '/';
}

// Landlock rules only add access; a rule on /tmp (the private tmpfs is writable) would also cover an installation that
// lives under /tmp, including its plain folders, because opens resolve the mounted subtree beneath the rule. The same is
// true of the other writable folders. Refuse such a layout: the four guarantees take priority over a portable install in
// /tmp. The self-test deliberately creates its tree in the working directory for this reason.
static int verify_rule_isolation(const struct hf_sandbox *sandbox)
{
    const char *protected[] = { sandbox->install, sandbox->session, sandbox->game, sandbox->prefix };
    for (int index = 0; index < sandbox->rule_count; index++)
    {
        if ((sandbox->rules[index].access & ~HF_ACCESS_RO) == 0) continue;
        for (size_t target = 0; target < sizeof(protected) / sizeof(protected[0]); target++)
            if (path_is_ancestor(sandbox->rules[index].path, protected[target]))
                return hf_fail_msg("a writable sandbox folder covers %s: %s; install and run outside /tmp, /dev/shm "
                    "and the runtime folder", protected[target], sandbox->rules[index].path);
    }
    return 0;
}

int hf_apply_landlock(struct hf_sandbox *sandbox)
{
    long abi = syscall(SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0) return hf_fail("Landlock (landlock_create_ruleset)");
    if (abi < 3) return hf_fail_msg("Landlock ABI %ld is below the required 3", abi);
    if (verify_rule_isolation(sandbox) != 0) return 1;

    struct landlock_ruleset_attr ruleset = { .handled_access_fs = HF_ACCESS_RW };
    int ruleset_fd = (int)syscall(SYS_landlock_create_ruleset, &ruleset, sizeof(ruleset), 0);
    if (ruleset_fd < 0) return hf_fail("landlock_create_ruleset");
    for (int index = 0; index < sandbox->rule_count; index++)
    {
        int parent = open(sandbox->rules[index].path, O_PATH | O_CLOEXEC);
        if (parent < 0)
        {
            close(ruleset_fd);
            return hf_fail(sandbox->rules[index].path);
        }
        struct landlock_path_beneath_attr beneath = {
            .allowed_access = sandbox->rules[index].access,
            .parent_fd = parent,
        };
        if (syscall(SYS_landlock_add_rule, ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &beneath, 0) != 0)
        {
            int saved = errno;
            close(parent);
            close(ruleset_fd);
            return hf_fail_msg("landlock_add_rule for %s: %s (errno %d)", sandbox->rules[index].path, strerror(saved), saved);
        }
        close(parent);
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    {
        close(ruleset_fd);
        return hf_fail("PR_SET_NO_NEW_PRIVS before Landlock");
    }
    if (syscall(SYS_landlock_restrict_self, ruleset_fd, 0) != 0)
    {
        int saved = errno;
        close(ruleset_fd);
        return hf_fail_msg("landlock_restrict_self: %s (errno %d)", strerror(saved), saved);
    }
    close(ruleset_fd);
    hf_note("Landlock ABI %ld over %d paths", abi, sandbox->rule_count);
    return 0;
}

int hf_apply_seccomp(void)
{
    struct sock_filter filter[HF_MAX_FILTER];
    size_t length = hf_filter_begin(filter, HF_MAX_FILTER);
    // The shared prelude denies x32 and clone3 (ENOSYS); this shared block denies clone with CLONE_NEW*. Phase 2 keeps
    // ordinary fork/clone, while phase 3 adds the process-creation, memory-access and signal rules here.
    if (hf_filter_deny_clone_namespaces(filter, &length, HF_MAX_FILTER) != 0) return hf_fail("building the seccomp filter");
    if (hf_filter_end(filter, length, HF_MAX_FILTER) != 0) return hf_fail("installing the seccomp filter");
    return 0;
}

struct hf_cap_header
{
    uint32_t version;
    int pid;
};

struct hf_cap_data
{
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
};

#define HF_CAP_VERSION_3 0x20080522U

int hf_drop_capabilities(void)
{
    int secure = SECBIT_NOROOT | SECBIT_NOROOT_LOCKED | SECBIT_NO_SETUID_FIXUP | SECBIT_NO_SETUID_FIXUP_LOCKED;
    if (prctl(PR_SET_SECUREBITS, secure, 0, 0, 0) != 0) return hf_fail("PR_SET_SECUREBITS");
    for (int capability = 0; capability <= 63; capability++)
        if (prctl(PR_CAPBSET_DROP, capability, 0, 0, 0) != 0 && errno != EINVAL && errno != EPERM) return hf_fail("PR_CAPBSET_DROP");
    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) != 0) return hf_fail("PR_CAP_AMBIENT_CLEAR_ALL");
    struct hf_cap_header header = { HF_CAP_VERSION_3, 0 };
    struct hf_cap_data data[2] = { { 0, 0, 0 }, { 0, 0, 0 } };
    if (syscall(SYS_capset, &header, data) != 0) return hf_fail("capset");
    if (syscall(SYS_capget, &header, data) != 0) return hf_fail("capget");
    if (data[0].effective != 0 || data[0].permitted != 0 || data[1].effective != 0 || data[1].permitted != 0)
        return hf_fail_msg("capabilities remain after the drop");
    for (int capability = 0; capability <= 63; capability++)
        if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, capability, 0, 0) == 1)
            return hf_fail_msg("an ambient capability remains after the drop: %d", capability);
    return 0;
}

static int write_denied(const char *path, bool *denied)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { *denied = true; return 0; }
    close(fd);
    unlink(path);
    *denied = false;
    return 0;
}

static int write_allowed(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return hf_fail(path);
    if (write_all(fd, "hf-sandbox", 10) != 0)
    {
        int saved = errno;
        close(fd);
        unlink(path);
        return hf_fail_msg("writing %s: %s (errno %d)", path, strerror(saved), saved);
    }
    close(fd);
    if (unlink(path) != 0) return hf_fail(path);
    return 0;
}

static int verify_allowed_writes(struct hf_sandbox *sandbox)
{
    char path[PATH_MAX];
    if (hf_join(path, sizeof(path), sandbox->game_out, ".hf-sandbox-allowed") != 0) return hf_fail("the check path");
    if (write_allowed(path) != 0) return 1;
    if (hf_join(path, sizeof(path), sandbox->merged, "pfx/drive_c") != 0) return hf_fail("the overlay's drive_c path");
    char overlay_check[PATH_MAX];
    if (hf_join(overlay_check, sizeof(overlay_check), path, ".hf-sandbox-overlay-check") != 0) return hf_fail("the overlay check path");
    return write_allowed(overlay_check);
}

static int verify_protected_writes(struct hf_sandbox *sandbox)
{
    struct { const char *base; const char *name; } denied[] =
    {
        { sandbox->game, ".hf-sandbox-denied" },          // the game copy is read-only
        { sandbox->install, ".hf-sandbox-denied" },       // the installation root is not a session folder
        { sandbox->session, ".hf-sandbox-denied" },       // the run folder is not the session game folder
        { sandbox->install, "profiles/.hf-sandbox-denied" }, // the installation's profiles are absent
        { sandbox->install, "mods/.hf-sandbox-denied" },     // and its mods
        { sandbox->install, "userdata/.hf-sandbox-denied" }, // and its user data
        { sandbox->prefix, "pfx/drive_c/.hf-sandbox-denied" }, // the real prefix is absent
        { "/etc", ".hf-sandbox-denied" },                  // the system view is read-only
        { "/", ".hf-sandbox-denied" },                     // the root tmpfs has no rule of its own
        { "/home", ".hf-sandbox-denied" },                 // no rule allows the host home
        { sandbox->host_home, ".hf-sandbox-denied" },       // the real home, also when it is /var/home
    };
    char path[PATH_MAX];
    for (size_t index = 0; index < sizeof(denied) / sizeof(denied[0]); index++)
    {
        if (snprintf(path, sizeof(path), "%s/%s", denied[index].base, denied[index].name) >= (int)sizeof(path))
            return hf_fail_msg("a sandbox check path is too long");
        bool is_denied = false;
        if (write_denied(path, &is_denied) != 0) return 1;
        if (!is_denied) return hf_fail_msg("the sandbox allowed a write outside the session folder: %s", path);
    }
    return 0;
}

static int verify_devices(void)
{
    const char *forbidden[] = { "/dev/input", "/dev/uinput", "/dev/kvm", "/dev/mem", "/dev/kmem" };
    for (size_t index = 0; index < sizeof(forbidden) / sizeof(forbidden[0]); index++)
    {
        struct stat info;
        if (lstat(forbidden[index], &info) == 0) return hf_fail_msg("a forbidden device is visible: %s", forbidden[index]);
    }
    int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null < 0) return hf_fail("/dev/null");
    close(null);
    return 0;
}

int hf_verify_view(struct hf_sandbox *sandbox)
{
    if (verify_allowed_writes(sandbox) != 0) return 1;
    if (verify_protected_writes(sandbox) != 0) return 1;
    return verify_devices();
}

int hf_close_inherited_fds(void)
{
    if (syscall(SYS_close_range, 3U, ~0U, 0) == 0) return 0;
    struct rlimit limit;
    unsigned long highest = 1024;
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY && limit.rlim_cur > 3 && limit.rlim_cur < 1UL << 20)
        highest = (unsigned long)limit.rlim_cur;
    for (unsigned long fd = 3; fd < highest; fd++) close((int)fd);
    return 0;
}

int hf_exec_command(struct hf_sandbox *sandbox)
{
    if (sandbox->command_count < 1 || sandbox->command[0] == NULL)
        return hf_fail_msg("no command was given after --");
    if (chdir(sandbox->game_out) != 0) return hf_fail("entering the session game folder");
    if (setenv("PWD", sandbox->game_out, 1) != 0) return hf_fail("setting PWD");
    // argv[0] must name the real script: Proton finds its own folder from $0, and a bare "proton" would make it look in
    // the session folder instead. The command line after argv[0] is unchanged.
    sandbox->command[0] = sandbox->command_path;
    hf_note("starting %s in the game domain", sandbox->command_path);
    execv(sandbox->command_path, sandbox->command);
    return hf_fail("exec");
}
