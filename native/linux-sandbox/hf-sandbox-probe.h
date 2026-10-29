// Shared definitions for the Linux sandbox probe (review-F1.md phase 1). The program is split only to keep each source
// file within the repository's module-size lint limit; it is one static executable, built from native/linux-sandbox/*.c.

#ifndef HF_SANDBOX_PROBE_H
#define HF_SANDBOX_PROBE_H

#include "hf-sandbox-common.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/mount.h>
#include <linux/openat2.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef O_PATH
#define O_PATH 010000000
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 02000000
#endif

#ifndef SYS_openat2
#define SYS_openat2 437
#endif
#ifndef SYS_close_range
#define SYS_close_range 436
#endif
#ifndef SYS_clone3
#define SYS_clone3 435
#endif
#ifndef SYS_pidfd_getfd
#define SYS_pidfd_getfd 438
#endif
#ifndef SYS_process_madvise
#define SYS_process_madvise 440
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
#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
#endif
#ifndef SYS_seccomp
#define SYS_seccomp 317
#endif

#ifndef RESOLVE_NO_SYMLINKS
#define RESOLVE_NO_SYMLINKS 0x04
#endif
#ifndef RESOLVE_BENEATH
#define RESOLVE_BENEATH 0x08
#endif
#ifndef AT_RECURSIVE
#define AT_RECURSIVE 0x8000
#endif
#ifndef MOUNT_ATTR_RDONLY
#define MOUNT_ATTR_RDONLY 0x00000001
#endif

#ifndef LANDLOCK_CREATE_RULESET_VERSION
#define LANDLOCK_CREATE_RULESET_VERSION (1U << 0)
#endif
#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER (1ULL << 13)
#endif
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14)
#endif

#ifndef SECCOMP_GET_ACTION_AVAIL
#define SECCOMP_GET_ACTION_AVAIL 2
#endif
#ifndef SECCOMP_RET_USER_NOTIF
#define SECCOMP_RET_USER_NOTIF 0x7fc00000U
#endif
#ifndef SECCOMP_RET_ERRNO
#define SECCOMP_RET_ERRNO 0x00050000U
#endif
#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#endif

#ifndef AUDIT_ARCH_X86_64
#define AUDIT_ARCH_X86_64 0xc000003eU
#endif

#define MAX_EVIDENCE 768
#define MAX_FILTER 128
#define CHILD_TIMEOUT_SECONDS 8
#define CANARY_TIMEOUT_SECONDS 20

enum
{
    ITEM_LANDLOCK,
    ITEM_SECCOMP_FILTER,
    ITEM_SECCOMP_USER_NOTIFICATION,
    ITEM_USERNS,
    ITEM_MOUNTNS,
    ITEM_PIDNS,
    ITEM_IPCNS,
    ITEM_NETNS,
    ITEM_OVERLAYFS,
    ITEM_READONLY_ROOT,
    ITEM_PRIVATE_TMP,
    ITEM_PRIVATE_SHM,
    ITEM_OPENAT2,
    ITEM_CLOSE_RANGE,
    ITEM_NO_NEW_PRIVS,
    ITEM_APPARMOR,
    ITEM_G1,
    ITEM_G1_LANDLOCK,
    ITEM_G1_ALLOWED,
    ITEM_G2,
    ITEM_G2_SELF_SIGNAL,
    ITEM_G3,
    ITEM_G3_THREADS,
    ITEM_G4,
    ITEM_COUNT
};

enum item_status
{
    STATUS_PENDING = 0,
    STATUS_PASS,
    STATUS_FAIL,
    STATUS_SKIP
};

struct shared_result
{
    int status[ITEM_COUNT];
    char evidence[ITEM_COUNT][MAX_EVIDENCE];
    int listener_port;
    pid_t victim_pid;
};

extern uid_t host_uid;
extern gid_t host_gid;
extern char temp_root[PATH_MAX];
extern char protected_dir[PATH_MAX];
extern char protected_canary[PATH_MAX];
extern char session_dir[PATH_MAX];
extern char landlock_rw[PATH_MAX];
extern char tmp_canary[PATH_MAX];
extern char shm_canary[PATH_MAX];
extern volatile unsigned char *victim_buffer;

void set_result(struct shared_result *shared, int item, int status, const char *format, ...);
bool make_shared_result(struct shared_result **shared_out);
bool make_victim_page(void);
void make_host_canaries(bool *have_tmp, bool *have_shm);
void child_failure(struct shared_result *shared, int item, int code, const char *what);
int run_child(struct shared_result *shared, int item, int seconds, int (*probe)(struct shared_result *));
int run_probe_child(struct shared_result *shared, int item, int (*probe)(struct shared_result *));
int make_root_readonly(const char *writable);

int probe_landlock(struct shared_result *shared);
int probe_seccomp_filter_child(struct shared_result *shared);
int probe_seccomp_user_notification(struct shared_result *shared);
int probe_userns_child(struct shared_result *shared);
int probe_mountns_child(struct shared_result *shared);
int probe_pidns_child(struct shared_result *shared);
int probe_ipcns_child(struct shared_result *shared);
int probe_netns_child(struct shared_result *shared);
int probe_overlayfs_child(struct shared_result *shared);
int probe_readonly_root_child(struct shared_result *shared);
int probe_private_tmp_child(struct shared_result *shared);
int probe_private_shm_child(struct shared_result *shared);
int probe_openat2(struct shared_result *shared);
int probe_close_range(struct shared_result *shared);
int probe_no_new_privs_child(struct shared_result *shared);
int probe_apparmor(struct shared_result *shared);

int run_canary(struct shared_result *shared);

#endif
