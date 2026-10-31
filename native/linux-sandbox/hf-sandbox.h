// hf-sandbox: the phase-2 Wine-domain filesystem sandbox (review-F1.md 4.1, 4.2, 5.1-5.4, 10 phase 2).
//
// One static, unprivileged x86_64 Linux program. It makes a new user and mount namespace, gives the game a per-session
// overlay prefix (the real prefix is a read-only lower layer), builds a fresh root with a tmpfs and read-only binds,
// chroots into it with pivot_root, then applies Landlock, no_new_privs, seccomp and a capability drop before execing the
// command. Every step fails closed: a failure prints the reason and exits non-zero; the command never runs unsandboxed.

#ifndef HF_SANDBOX_H
#define HF_SANDBOX_H

#include "hf-sandbox-common.h"

#include <linux/landlock.h>
#include <linux/securebits.h>

#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER (1ULL << 13)
#endif
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14)
#endif
#ifndef LANDLOCK_CREATE_RULESET_VERSION
#define LANDLOCK_CREATE_RULESET_VERSION (1U << 0)
#endif
#ifndef SECCOMP_RET_ERRNO
#define SECCOMP_RET_ERRNO 0x00050000U
#endif
#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#endif
#ifndef SECCOMP_RET_ALLOW
#define SECCOMP_RET_ALLOW 0x7fff0000U
#endif
#ifndef SECCOMP_MODE_FILTER
#define SECCOMP_MODE_FILTER 2
#endif
#ifndef SYS_mount_setattr
#define SYS_mount_setattr 442
#endif
#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#endif
#ifndef SYS_landlock_add_rule
#define SYS_landlock_add_rule 445
#endif
#ifndef SYS_landlock_restrict_self
#define SYS_landlock_restrict_self 446
#endif
#ifndef SYS_capset
#define SYS_capset 126
#endif
#ifndef SYS_capget
#define SYS_capget 125
#endif

#define HF_MAX_RULES 128
#define HF_MAX_FILTER 128
#define HF_MAX_COMMAND 256
#define HF_MAX_EXTRA_BINDS 64
#define HF_MAX_STEAM_RUNTIMES 4
#define HF_RUNTIME_SOCKETS 8
#define HF_MAX_CREDENTIAL_BYTES 65536
#define HF_SANDBOX_UID 0

#define HF_ACCESS_RO (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_EXECUTE)
#define HF_ACCESS_FILE_RO (LANDLOCK_ACCESS_FS_READ_FILE)
#define HF_ACCESS_PROTON_LOCK (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE)
#define HF_ACCESS_DEVICE (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE)
#define HF_ACCESS_RW (LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE | \
    LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE | \
    LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG | \
    LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK | \
    LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_REFER | LANDLOCK_ACCESS_FS_TRUNCATE)

struct hf_rule
{
    char path[PATH_MAX];
    uint64_t access;
};

struct hf_sandbox
{
    char session[PATH_MAX];       // the run folder: <install>/logs/sessions/<id>
    char prefix[PATH_MAX];        // the real Steam compatdata prefix; contains pfx/
    char install[PATH_MAX];       // the installation, derived from the run folder
    char game[PATH_MAX];          // <install>/game, the game copy (read-only in the view)
    char game_out[PATH_MAX];      // <session>/game, the only session folder the game writes
    char root[PATH_MAX];          // where the new root tmpfs is mounted before pivot_root
    char overlay_root[PATH_MAX];  // <game_out>/prefix
    char sandbox_state[PATH_MAX]; // <session>/sandbox: upper/work, outside gameOut
    char upper[PATH_MAX];
    char work[PATH_MAX];
    char merged[PATH_MAX];
    char overlay_install[PATH_MAX]; // the installation inside merged when it is under the prefix
    char home[PATH_MAX];
    char cache[PATH_MAX];
    char config[PATH_MAX];
    char data[PATH_MAX];
    char state[PATH_MAX];
    char runtime[PATH_MAX];       // the host's XDG_RUNTIME_DIR when set, else /run/user/<uid>, inside the view
    char host_runtime[PATH_MAX];  // the host's XDG_RUNTIME_DIR, the source of the allowed sockets
    char host_home[PATH_MAX];     // the player's real home, resolved before the view
    char proton_root[PATH_MAX];   // the Proton installation, read-only except dist.lock
    char proton_lock[PATH_MAX];   // <proton_root>/dist.lock, the one writable file in the Proton folder
    char steam_root[PATH_MAX];    // STEAM_COMPAT_CLIENT_INSTALL_PATH
    char vulkan_layers[PATH_MAX]; // the host's XDG_DATA_HOME/vulkan/implicit_layer.d, read-only
    char lsfg_config[PATH_MAX];   // the player's lsfg-vk/conf.toml, copied into the session config
    char shader_cache[PATH_MAX];  // Steam's per-game shader cache, writable (G.1's reviewed exception)
    char command_path[PATH_MAX];  // the resolved Linux executable
    char x11_socket[PATH_MAX];    // resolved host /tmp/.X11-unix/X<n>, when DISPLAY names a local display
    char x11_name[64];            // the socket name the game expects (X<n>), not a link's target name
    char wayland_socket[PATH_MAX]; // the resolved service path, never the $XDG_RUNTIME_DIR link
    char wayland_name[256];       // the socket name the game expects from WAYLAND_DISPLAY
    char pipewire_socket[PATH_MAX];
    char pulse_socket[PATH_MAX];
    char pulse_cookie[PATH_MAX];
    char xauthority[PATH_MAX];
    char selftest_tmp_canary[PATH_MAX];   // the self-test's host-side /tmp canary
    char selftest_shm_canary[PATH_MAX];   // and its /dev/shm canary
    char selftest_deleted_source[PATH_MAX]; // a temp file the self-test binds and then unlinks
    char selftest_deleted_target[PATH_MAX]; // the path left pointing at the unlinked file
    char selftest_listing_canary[PATH_MAX]; // a host file beside the installation's bare folders, never listed in the view
    char **extra_binds;                   // existing files/folders exposed read-only at their own paths
    int extra_bind_count;
    char **steam_runtimes;                // the Steam tool folders Proton's manifest requires (--steam-runtime), read-only
    int steam_runtime_count;
    char **command;               // argv after --
    int command_count;
    uid_t host_uid;
    gid_t host_gid;
    bool self_test;
    bool install_in_prefix;       // true when overlay_install masks the installation inside merged
    struct hf_rule rules[HF_MAX_RULES];
    int rule_count;
};

// Diagnostics. Every failure is one line on stderr, and every caller returns non-zero without execing the command.
int hf_fail(const char *what);
int hf_fail_msg(const char *format, ...);
void hf_note(const char *format, ...);

// Paths.
int hf_prepare_paths(struct hf_sandbox *sandbox, const char *session, const char *prefix);
int hf_prepare_selftest_paths(struct hf_sandbox *sandbox, const char *temporary);
int hf_discover_environment(struct hf_sandbox *sandbox, char **argv, int argc);
int hf_discover_runtime_sockets(struct hf_sandbox *sandbox);
int hf_resolve_command(struct hf_sandbox *sandbox, const char *command);
int hf_add_rule(struct hf_sandbox *sandbox, const char *path, uint64_t access);

// The game domain: session dirs, overlay prefix, namespaces, the root view and pivot_root.
int hf_create_session_dirs(struct hf_sandbox *sandbox);
int hf_prepare_session_home(struct hf_sandbox *sandbox);
int hf_require_empty_dir(const char *path, const char *what);
int hf_mount_overlay(struct hf_sandbox *sandbox);
int hf_mask_overlay_install(struct hf_sandbox *sandbox);
int hf_enter_namespaces(struct hf_sandbox *sandbox);
int hf_build_root_view(struct hf_sandbox *sandbox);
bool hf_extra_bind_allowed(const struct hf_sandbox *sandbox, const char *path);
int hf_pivot_into_root(struct hf_sandbox *sandbox);
int hf_apply_environment(struct hf_sandbox *sandbox);
int hf_check_vulkan_layers(const struct hf_sandbox *sandbox);
int hf_copy_lsfg_config(const struct hf_sandbox *sandbox);

// The boundaries and their proof.
int hf_apply_landlock(struct hf_sandbox *sandbox);
int hf_apply_seccomp(void);
int hf_drop_capabilities(void);
int hf_verify_view(struct hf_sandbox *sandbox);
int hf_close_inherited_fds(void);
int hf_exec_command(struct hf_sandbox *sandbox);

// Self-test: the real view, the real guard, and the phase-1 G1 canaries. directory is the working folder (or NULL for
// the current folder); it must not be under /tmp, /dev/shm, /run (a drive under /run/media is fine) or /usr.
int hf_run_self_test(const char *directory);

#endif
