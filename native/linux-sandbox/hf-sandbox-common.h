// Helpers shared by the Linux sandbox probe (phase 1) and hf-sandbox (phase 2). They are plain POSIX/Linux calls with no
// dependency beyond the static musl build; every program in this folder is one static executable built by the pinned Zig.

#ifndef HF_SANDBOX_COMMON_H
#define HF_SANDBOX_COMMON_H

#define _GNU_SOURCE 1

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
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

#ifndef SYS_close_range
#define SYS_close_range 436
#endif
#ifndef SYS_openat2
#define SYS_openat2 437
#endif
#ifndef SYS_clone3
#define SYS_clone3 435
#endif

// x32 sets bit 30 in every syscall number. The seccomp number list below is x86_64-only; denying the whole x32 range
// stops an x32 mount/unshare from choosing a number the list does not have.
#ifndef __X32_SYSCALL_BIT
#define __X32_SYSCALL_BIT 0x40000000U
#endif
#ifndef CLONE_NEWTIME
#define CLONE_NEWTIME 0x00000080U
#endif
#ifndef CLONE_NEWCGROUP
#define CLONE_NEWCGROUP 0x02000000U
#endif
#define HF_CLONE_NEW_MASK (CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | \
    CLONE_NEWPID | CLONE_NEWNET | CLONE_NEWTIME)

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

// A path join that reports truncation instead of silently cutting the path. Both programs build security-relevant paths.
int hf_join(char *out, size_t size, const char *base, const char *name);
int hf_join3(char *out, size_t size, const char *first, const char *second, const char *third);

void joinf(char *out, size_t size, const char *base, const char *name);
int make_dir(const char *path);
int make_parent_dirs(const char *path);
int write_all(int fd, const void *data, size_t length);
int write_file(const char *path, const char *text);
int read_text(const char *path, char *out, size_t size);
int path_exists(const char *path);
void remove_tree(const char *path);
int enter_user_namespace(void);
int private_mount_tree(void);
int wait_bounded(pid_t pid, int seconds, int *status_out);

// The seccomp filter builder both programs use: small emit helpers, one denial, and the no_new_privs install.
void hf_filter_stmt(struct sock_filter *filter, size_t *length, unsigned short code, unsigned int value);
size_t hf_filter_jump(struct sock_filter *filter, size_t *length, unsigned short code, unsigned int value);
void hf_filter_deny(struct sock_filter *filter, size_t *length, unsigned int number);
void hf_filter_deny_errno(struct sock_filter *filter, size_t *length, unsigned int number, unsigned int error);
// Denies clone() when arg0 has any CLONE_NEW* bit. It restores A=nr before falling through, so the probe's clone/thread
// and kill rules after it keep working; call it before those rules and before hf_filter_end.
int hf_filter_deny_clone_namespaces(struct sock_filter *filter, size_t *length, size_t capacity);
size_t hf_filter_begin(struct sock_filter *filter, size_t capacity);
int hf_filter_end(struct sock_filter *filter, size_t length, size_t capacity);

#endif
