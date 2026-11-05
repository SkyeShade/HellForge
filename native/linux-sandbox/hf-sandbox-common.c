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

// A sysfs capability bitmap ("abs", "key", "rel"): words of unsigned long in hex, the most significant first, as the input
// core prints them. bits gets the words least significant first; a missing file is an empty bitmap.
#define HF_PAD_WORDS 16
static void read_capabilities(const char *device_dir, const char *name, unsigned long bits[HF_PAD_WORDS])
{
    memset(bits, 0, sizeof(unsigned long) * HF_PAD_WORDS);
    char path[PATH_MAX], text[1024];
    if (snprintf(path, sizeof(path), "%s/capabilities/%s", device_dir, name) >= (int)sizeof(path)) return;
    if (read_text(path, text, sizeof(text)) != 0) return;
    unsigned long words[HF_PAD_WORDS * 2];
    size_t count = 0;
    char *cursor = text;
    while (*cursor != '\0' && count < sizeof(words) / sizeof(words[0]))
    {
        while (*cursor == ' ' || *cursor == '\n') cursor++;
        if (*cursor == '\0') break;
        char *end = NULL;
        words[count++] = strtoul(cursor, &end, 16);
        if (end == cursor) break;
        cursor = end;
    }
    for (size_t index = 0; index < count && index < HF_PAD_WORDS; index++) bits[index] = words[count - 1 - index];
}

static bool bit_set(const unsigned long bits[HF_PAD_WORDS], unsigned int bit)
{
    size_t word = bit / (8 * sizeof(unsigned long));
    return word < HF_PAD_WORDS && (bits[word] >> (bit % (8 * sizeof(unsigned long))) & 1UL) != 0;
}

static bool any_bit(const unsigned long bits[HF_PAD_WORDS], unsigned int first, unsigned int last)
{
    for (unsigned int bit = first; bit <= last; bit++)
        if (bit_set(bits, bit)) return true;
    return false;
}

bool hf_pad_event(const char *device_dir)
{
    unsigned long key[HF_PAD_WORDS], abs_axes[HF_PAD_WORDS], rel[HF_PAD_WORDS];
    read_capabilities(device_dir, "key", key);
    read_capabilities(device_dir, "abs", abs_axes);
    read_capabilities(device_dir, "rel", rel);
    // BTN_JOYSTICK (0x120) to BTN_THUMBR (0x13e): joystick and gamepad buttons; ABS_X (0) to ABS_MAX (0x3f): any axis.
    bool controller = any_bit(key, 0x120, 0x13e) && any_bit(abs_axes, 0x00, 0x3f);
    // KEY_ESC (1) to KEY_F12 (0x58): the keys of a keyboard; REL_X and REL_Y: a pointer.
    bool keyboard = any_bit(key, 0x01, 0x58);
    bool pointer = bit_set(rel, 0x00) || bit_set(rel, 0x01);
    return controller && !keyboard && !pointer;
}

// The HID report descriptor's short items that say what a device is: the global Usage Page, the local Usage (a 4-byte one
// carries its page) and Collection / End Collection. An application collection at the top level is what the device is.
struct hid_scan
{
    unsigned int page, usage, usage_page, depth;
    bool usage_set, controller, other;
};

struct hid_item
{
    unsigned int type, tag, size, value;
};

// The next item at *at; a long item is skipped whole. False at the end or on a truncated item.
static bool hid_next(const unsigned char *descriptor, ssize_t length, ssize_t *at, struct hid_item *item)
{
    while (*at < length && descriptor[*at] == 0xFE) // a long item: its data size, then its tag, then the data
    {
        if (*at + 2 >= length) return false;
        *at += 3 + descriptor[*at + 1];
    }
    if (*at >= length) return false;
    unsigned char prefix = descriptor[*at];
    static const unsigned int sizes[] = { 0, 1, 2, 4 };
    item->size = sizes[prefix & 3];
    item->type = (prefix >> 2) & 3;
    item->tag = prefix >> 4;
    if (*at + 1 + (ssize_t)item->size > length) return false;
    item->value = 0;
    for (unsigned int index = 0; index < item->size; index++)
        item->value |= (unsigned int)descriptor[*at + 1 + index] << (8 * index);
    *at += 1 + item->size;
    return true;
}

// A Collection item: a top-level application collection names the device's kind.
static void hid_collection(struct hid_scan *scan, unsigned int kind)
{
    bool application = scan->depth == 0 && kind == 1 && scan->usage_set;
    scan->depth++;
    if (!application) return;
    bool desktop = scan->usage_page == 0x01;
    if (desktop && (scan->usage == 0x04 || scan->usage == 0x05 || scan->usage == 0x08)) scan->controller = true;
    // a mouse, keyboard, keypad or digitizer (touch screen, pen)
    else if ((desktop && (scan->usage == 0x02 || scan->usage == 0x06 || scan->usage == 0x07)) || scan->usage_page == 0x0D)
        scan->other = true;
}

static void hid_apply(struct hid_scan *scan, const struct hid_item *item)
{
    if (item->type == 1 && item->tag == 0) scan->page = item->value; // Usage Page
    else if (item->type == 2 && item->tag == 0 && !scan->usage_set) // the first Usage before the next main item
    {
        scan->usage = item->size == 4 ? item->value & 0xFFFF : item->value;
        scan->usage_page = item->size == 4 ? item->value >> 16 : scan->page;
        scan->usage_set = true;
    }
    else if (item->type == 0)
    {
        if (item->tag == 0xA) hid_collection(scan, item->value);
        else if (item->tag == 0xC && scan->depth > 0) scan->depth--; // End Collection
        scan->usage_set = false; // a main item ends its local items
    }
}

bool hf_pad_hidraw(const char *device_dir)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/report_descriptor", device_dir) >= (int)sizeof(path)) return false;
    unsigned char descriptor[4096];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t length = read(fd, descriptor, sizeof(descriptor));
    close(fd);
    struct hid_scan scan;
    memset(&scan, 0, sizeof(scan));
    struct hid_item item;
    ssize_t at = 0;
    while (hid_next(descriptor, length, &at, &item)) hid_apply(&scan, &item);
    // A truncated descriptor stops short of its end: not trusted as a controller.
    return length > 0 && at == length && scan.controller && !scan.other;
}

// A character device's sysfs entry (/sys/dev/char/<major>:<minor>, resolved), its subsystem's name and its kernel name.
static bool node_sysfs(dev_t device, char *resolved, char *kind, size_t kind_size, char *base, size_t base_size)
{
    char link[PATH_MAX], subsystem[PATH_MAX], target[PATH_MAX];
    snprintf(link, sizeof(link), "/sys/dev/char/%u:%u", major(device), minor(device));
    if (realpath(link, resolved) == NULL) return false;
    if (hf_join(subsystem, sizeof(subsystem), resolved, "subsystem") != 0) return false;
    ssize_t got = readlink(subsystem, target, sizeof(target) - 1);
    if (got <= 0) return false;
    target[got] = '\0';
    const char *last = strrchr(target, '/');
    if (snprintf(kind, kind_size, "%s", last != NULL ? last + 1 : target) >= (int)kind_size) return false;
    last = strrchr(resolved, '/');
    return snprintf(base, base_size, "%s", last != NULL ? last + 1 : resolved) < (int)base_size;
}

bool hf_input_node(dev_t device)
{
    char resolved[PATH_MAX], kind[64], base[64];
    // The input core's major is 13 whatever sysfs says. Input and hidraw devices always have a sysfs entry, so a node it
    // cannot place (NVIDIA's mknod-made nodes) is neither; such an input node would still be refused by its major.
    if (major(device) == 13) return true;
    if (!node_sysfs(device, resolved, kind, sizeof(kind), base, sizeof(base))) return false;
    return strcmp(kind, "input") == 0 || strcmp(kind, "hidraw") == 0;
}

bool hf_pad_node(dev_t device, char *name, size_t name_size)
{
    char resolved[PATH_MAX], kind[64], base[64], device_dir[PATH_MAX];
    if (!node_sysfs(device, resolved, kind, sizeof(kind), base, sizeof(base))) return false;
    if (name != NULL && snprintf(name, name_size, "%s", base) >= (int)name_size) return false;
    if (hf_join(device_dir, sizeof(device_dir), resolved, "device") != 0) return false;
    if (strcmp(kind, "input") == 0) return strncmp(base, "event", 5) == 0 && hf_pad_event(device_dir);
    if (strcmp(kind, "hidraw") == 0) return hf_pad_hidraw(device_dir);
    return false;
}

bool hf_dot_entry(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

enum hf_dev_entry hf_dev_entry(const char *folder, const char *name, bool input_folder)
{
    char path[PATH_MAX], kernel[64];
    struct stat info;
    if (hf_join(path, sizeof(path), folder, name) != 0 || lstat(path, &info) != 0 || !S_ISCHR(info.st_mode))
        return input_folder ? HF_DEV_REFUSED : HF_DEV_OTHER;
    if (!input_folder && !hf_input_node(info.st_rdev)) return HF_DEV_OTHER;
    const char *prefix = input_folder ? "event" : "hidraw";
    bool controller = hf_pad_node(info.st_rdev, kernel, sizeof(kernel)) && strcmp(kernel, name) == 0 &&
        strncmp(kernel, prefix, strlen(prefix)) == 0;
    return controller ? HF_DEV_CONTROLLER : HF_DEV_REFUSED;
}
