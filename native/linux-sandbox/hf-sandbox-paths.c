// Argument, path and environment discovery for hf-sandbox. See hf-sandbox.h.

#include "hf-sandbox.h"

#include <math.h>
#include <stdarg.h>
#include <sys/socket.h>

int hf_fail(const char *what)
{
    int saved = errno;
    fprintf(stderr, "hf-sandbox: FAIL: %s: %s (errno %d)\n", what, strerror(saved), saved);
    return 1;
}

int hf_fail_msg(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fputs("hf-sandbox: FAIL: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
    return 1;
}

void hf_note(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fputs("hf-sandbox: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
}

static int copy_path(char *out, size_t size, const char *value, const char *what)
{
    int written = snprintf(out, size, "%s", value);
    if (written < 0 || (size_t)written >= size) return hf_fail_msg("the %s path is too long", what);
    return 0;
}

static int resolve_existing(const char *path, char *out, size_t size, const char *what)
{
    char *resolved = realpath(path, NULL);
    if (resolved == NULL) return hf_fail(what);
    int written = snprintf(out, size, "%s", resolved);
    free(resolved);
    if (written < 0 || (size_t)written >= size) return hf_fail_msg("the %s path is too long", what);
    return 0;
}

static bool is_directory(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static bool path_within(const char *ancestor, const char *path)
{
    size_t length = strlen(ancestor);
    while (length > 1 && ancestor[length - 1] == '/') length--;
    if (length == 1 && ancestor[0] == '/') return path[0] == '/';
    if (length == 0 || strncmp(ancestor, path, length) != 0) return false;
    return path[length] == '\0' || path[length] == '/';
}

// A system folder may itself be a symlink (Fedora Atomic has /home -> /var/home and /opt -> /var/opt). The bind is made
// at the system name, but its content is the resolved folder; compare both names with the realpath'd install/prefix.
static bool system_covers(const char *system, const char *path)
{
    if (path_within(system, path)) return true;
    char resolved[PATH_MAX];
    if (realpath(system, resolved) == NULL) return false;
    return path_within(resolved, path);
}

static bool is_regular(const char *path)
{
    struct stat info;
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

// A service root may be a link itself (/run/user/1000 can be reached through one); compare the real folder too.
static bool service_root_covers(const char *root, const char *path)
{
    if (root[0] == '\0') return false;
    if (path_within(root, path)) return true;
    char resolved[PATH_MAX];
    if (realpath(root, resolved) == NULL) return false;
    return path_within(resolved, path);
}

static bool runtime_service_allowed(const struct hf_sandbox *sandbox, const char *path)
{
    return service_root_covers(sandbox->host_runtime, path) ||
        service_root_covers("/run/pressure-vessel", path) ||
        service_root_covers("/tmp/.X11-unix", path);
}

// A pressure-vessel runtime folder holds relative links (wayland-0 -> ../../pressure-vessel/wayland-0,
// pulse -> ../../pressure-vessel/pulse). The link name is not a mount source: resolve it here, require the resolved
// object to be the type the service needs, and keep a socket only inside the runtime folders. A link that resolves
// elsewhere is dropped with a note; it is never followed into another folder.
static int resolve_runtime_source(const struct hf_sandbox *sandbox, const char *what, const char *source,
    bool want_socket, char *out, size_t size)
{
    out[0] = '\0';
    if (source == NULL || source[0] == '\0') return 0;
    char *resolved = realpath(source, NULL);
    if (resolved == NULL)
    {
        hf_note("%s %s is gone; not binding it", what, source);
        return 0;
    }
    struct stat info;
    if (lstat(resolved, &info) != 0)
    {
        hf_note("%s %s is gone; not binding it", what, resolved);
        free(resolved);
        return 0;
    }
    if (want_socket ? !S_ISSOCK(info.st_mode) : !S_ISREG(info.st_mode))
    {
        hf_note("%s %s is not a %s; not binding it", what, resolved, want_socket ? "socket" : "regular file");
        free(resolved);
        return 0;
    }
    if (want_socket && !runtime_service_allowed(sandbox, resolved))
    {
        hf_note("%s %s resolves outside the runtime folders to %s; not binding it", what, source, resolved);
        free(resolved);
        return 0;
    }
    int result = copy_path(out, size, resolved, what);
    free(resolved);
    return result;
}

int hf_add_rule(struct hf_sandbox *sandbox, const char *path, uint64_t access)
{
    for (int index = 0; index < sandbox->rule_count; index++)
        if (strcmp(sandbox->rules[index].path, path) == 0)
        {
            sandbox->rules[index].access |= access;
            return 0;
        }
    if (sandbox->rule_count >= HF_MAX_RULES) return hf_fail_msg("too many paths for the Landlock rules");
    struct hf_rule *rule = &sandbox->rules[sandbox->rule_count++];
    if (snprintf(rule->path, sizeof(rule->path), "%s", path) >= (int)sizeof(rule->path))
        return hf_fail_msg("a Landlock path is too long");
    rule->access = access;
    return 0;
}

static int derive_install(struct hf_sandbox *sandbox)
{
    char work[PATH_MAX];
    if (copy_path(work, sizeof(work), sandbox->session, "session") != 0) return 1;
    char *slash = strrchr(work, '/');
    if (slash == NULL || slash[1] == '\0') return hf_fail_msg("the session folder does not look like <install>/logs/sessions/<run>");
    slash = strrchr(work, '/');
    if (slash == NULL) return hf_fail_msg("the session folder is not below logs/sessions");
    *slash = '\0';
    slash = strrchr(work, '/');
    if (slash == NULL || strcmp(slash + 1, "sessions") != 0)
        return hf_fail_msg("the session folder is not below logs/sessions");
    *slash = '\0';
    slash = strrchr(work, '/');
    if (slash == NULL || strcmp(slash + 1, "logs") != 0)
        return hf_fail_msg("the session folder is not below logs/sessions");
    *slash = '\0';
    if (work[0] == '\0') return hf_fail_msg("the installation folder could not be derived from the session folder");
    return copy_path(sandbox->install, sizeof(sandbox->install), work, "installation");
}

static int prepare_session_paths(struct hf_sandbox *sandbox)
{
    if (hf_join(sandbox->game, sizeof(sandbox->game), sandbox->install, "game") != 0) return hf_fail("the game copy path");
    if (hf_join(sandbox->game_out, sizeof(sandbox->game_out), sandbox->session, "game") != 0) return hf_fail("the session game folder");
    if (hf_join(sandbox->root, sizeof(sandbox->root), sandbox->session, "root") != 0) return hf_fail("the new root path");
    // The overlay's own upper/work must stay outside gameOut: the game can write gameOut, and writing the upper behind
    // overlayfs would corrupt the per-session view. They live in <session>/sandbox, which is never bound into the view.
    if (hf_join(sandbox->sandbox_state, sizeof(sandbox->sandbox_state), sandbox->session, "sandbox") != 0)
        return hf_fail("the sandbox state path");
    if (hf_join(sandbox->upper, sizeof(sandbox->upper), sandbox->sandbox_state, "upper") != 0) return hf_fail("the overlay upper path");
    if (hf_join(sandbox->work, sizeof(sandbox->work), sandbox->sandbox_state, "work") != 0) return hf_fail("the overlay work path");
    if (hf_join(sandbox->overlay_root, sizeof(sandbox->overlay_root), sandbox->game_out, "prefix") != 0) return hf_fail("the overlay path");
    return hf_join(sandbox->merged, sizeof(sandbox->merged), sandbox->overlay_root, "merged") == 0
        ? 0 : hf_fail("the overlay merged path");
}

static int prepare_home_paths(struct hf_sandbox *sandbox)
{
    if (hf_join(sandbox->home, sizeof(sandbox->home), sandbox->game_out, "home") != 0) return hf_fail("the session home path");
    if (hf_join(sandbox->cache, sizeof(sandbox->cache), sandbox->game_out, "cache") != 0) return hf_fail("the session cache path");
    if (hf_join(sandbox->config, sizeof(sandbox->config), sandbox->game_out, "config") != 0) return hf_fail("the session config path");
    if (hf_join(sandbox->data, sizeof(sandbox->data), sandbox->game_out, "data") != 0) return hf_fail("the session data path");
    return hf_join(sandbox->state, sizeof(sandbox->state), sandbox->game_out, "state") == 0
        ? 0 : hf_fail("the session state path");
}

static int prepare_runtime_path(struct hf_sandbox *sandbox)
{
    // Inside a user namespace started by hellforge-offline.sh the player is uid 0, but the host runtime and every socket
    // path is /run/user/<host uid>. Use the host XDG_RUNTIME_DIR's path when it is set and absolute, so Wine, PulseAudio
    // and Wayland see the host's path; otherwise use the old fallback.
    if (sandbox->host_runtime[0] == '/')
        return copy_path(sandbox->runtime, sizeof(sandbox->runtime), sandbox->host_runtime, "runtime");
    if (snprintf(sandbox->runtime, sizeof(sandbox->runtime), "/run/user/%u", (unsigned)sandbox->host_uid) >= (int)sizeof(sandbox->runtime))
        return hf_fail_msg("the runtime path is too long");
    return 0;
}

static int prepare_overlay_install_path(struct hf_sandbox *sandbox)
{
    // When the installation is inside the real prefix, its lower-layer copy would show profiles/mods/userdata through
    // C:. overlay_install is where hf_mask_overlay_install puts an empty tmpfs with only the game copy.
    sandbox->install_in_prefix = false;
    sandbox->overlay_install[0] = '\0';
    if (strcmp(sandbox->install, sandbox->prefix) == 0 || !path_within(sandbox->prefix, sandbox->install)) return 0;
    const char *relative = sandbox->install + strlen(sandbox->prefix);
    if (*relative == '/') relative++;
    if (*relative == '\0') return 0;
    if (hf_join(sandbox->overlay_install, sizeof(sandbox->overlay_install), sandbox->merged, relative) != 0)
        return hf_fail("the installation's overlay path");
    sandbox->install_in_prefix = true;
    return 0;
}

static int prepare_common_paths(struct hf_sandbox *sandbox)
{
    if (prepare_session_paths(sandbox) != 0) return 1;
    if (prepare_home_paths(sandbox) != 0) return 1;
    if (prepare_runtime_path(sandbox) != 0) return 1;
    return prepare_overlay_install_path(sandbox);
}

static int validate_installation(struct hf_sandbox *sandbox)
{
    char marker[PATH_MAX];
    if (hf_join(marker, sizeof(marker), sandbox->install, "hellforge-install.json") != 0) return hf_fail("the installation marker path");
    if (!is_regular(marker)) return hf_fail_msg("%s is not a HellForge installation (no hellforge-install.json)", sandbox->install);
    if (!is_directory(sandbox->game)) return hf_fail_msg("the game copy is missing: %s", sandbox->game);
    if (strcmp(sandbox->install, sandbox->prefix) == 0)
        return hf_fail_msg("the installation must not be the prefix root: %s", sandbox->install);
    // /usr and /etc are bound read-only as a whole for the loader/NSS. An installation below them would still be visible
    // there, including its profiles, mods and userdata, so the view could not make those absent. Refuse it; the normal
    // install is under the home (on Fedora Atomic that is /var/home, which this view does not bind whole).
    if (system_covers("/usr", sandbox->install) || system_covers("/etc", sandbox->install))
        return hf_fail_msg("the installation is under a system folder the sandbox binds read-only, so profiles/mods/userdata "
            "could not be hidden: %s", sandbox->install);
    if (system_covers("/usr", sandbox->prefix) || system_covers("/etc", sandbox->prefix))
        return hf_fail_msg("the real prefix is under a system folder the sandbox binds read-only, so it would not be absent: %s",
            sandbox->prefix);
    if (system_covers("/usr", sandbox->host_home) || system_covers("/etc", sandbox->host_home))
        return hf_fail_msg("the real home is under a system folder the sandbox binds read-only, so it would not be absent: %s",
            sandbox->host_home);

    char wine_prefix[PATH_MAX];
    if (hf_join(wine_prefix, sizeof(wine_prefix), sandbox->prefix, "pfx") != 0) return hf_fail("the prefix's pfx path");
    if (!is_directory(wine_prefix))
        return hf_fail_msg("--prefix must be the Steam compatdata folder that holds pfx/ (STEAM_COMPAT_DATA_PATH), not the Wine prefix itself: %s",
            sandbox->prefix);
    return 0;
}

static int discover_host_home(struct hf_sandbox *sandbox)
{
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') { sandbox->host_home[0] = '\0'; return 0; }
    // Resolve before the view exists: on Fedora Atomic /home is a link, and after pivot_root realpath("/home/x") would
    // name an empty tmpfs folder rather than the real home the canary must try.
    char *resolved = realpath(home, NULL);
    const char *value = resolved == NULL ? home : resolved;
    int result = copy_path(sandbox->host_home, sizeof(sandbox->host_home), value, "home");
    free(resolved);
    return result;
}

static int find_host_runtime(struct hf_sandbox *sandbox)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime != NULL && runtime[0] != '\0' && is_directory(runtime))
        return copy_path(sandbox->host_runtime, sizeof(sandbox->host_runtime), runtime, "runtime");
    char fallback[PATH_MAX];
    if (snprintf(fallback, sizeof(fallback), "/run/user/%u", (unsigned)sandbox->host_uid) >= (int)sizeof(fallback))
        return hf_fail_msg("the runtime path is too long");
    if (is_directory(fallback)) return copy_path(sandbox->host_runtime, sizeof(sandbox->host_runtime), fallback, "runtime");
    return 0;
}

int hf_prepare_paths(struct hf_sandbox *sandbox, const char *session, const char *prefix)
{
    sandbox->host_uid = getuid();
    sandbox->host_gid = getgid();
    if (resolve_existing(session, sandbox->session, sizeof(sandbox->session), "session folder") != 0) return 1;
    if (resolve_existing(prefix, sandbox->prefix, sizeof(sandbox->prefix), "prefix") != 0) return 1;
    if (derive_install(sandbox) != 0) return 1;
    // The runtime must be discovered before prepare_common_paths: when the caller is uid 0 in hellforge-offline.sh's
    // user namespace, the host's /run/user/<uid> path comes from XDG_RUNTIME_DIR, not getuid().
    if (find_host_runtime(sandbox) != 0) return 1;
    if (prepare_common_paths(sandbox) != 0) return 1;
    if (discover_host_home(sandbox) != 0) return 1;
    return validate_installation(sandbox);
}

int hf_prepare_selftest_paths(struct hf_sandbox *sandbox, const char *temporary)
{
    sandbox->host_uid = getuid();
    sandbox->host_gid = getgid();
    sandbox->self_test = true;
    if (snprintf(sandbox->prefix, sizeof(sandbox->prefix), "%s/prefix", temporary) >= (int)sizeof(sandbox->prefix))
        return hf_fail_msg("the self-test prefix path is too long");
    // The default installation layout: the installation is inside the prefix the overlay lower is made from, at the
    // exact path Proton gives the game as C:\users\steamuser\Documents\HellForge.
    if (snprintf(sandbox->install, sizeof(sandbox->install),
        "%s/prefix/pfx/drive_c/users/steamuser/Documents/HellForge", temporary) >= (int)sizeof(sandbox->install))
        return hf_fail_msg("the self-test installation path is too long");
    if (hf_join3(sandbox->session, sizeof(sandbox->session), sandbox->install, "logs/sessions", "self-test") != 0)
        return hf_fail("the self-test session path");
    // The self-test uses the same runtime path source as a session: its fixture sockets live under XDG_RUNTIME_DIR.
    if (find_host_runtime(sandbox) != 0) return 1;
    if (prepare_common_paths(sandbox) != 0) return 1;
    return discover_host_home(sandbox);
}

static int env_path(const char *name, char *out, size_t size)
{
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') { out[0] = '\0'; return 0; }
    return copy_path(out, size, value, name);
}

static int discover_wayland_socket(struct hf_sandbox *sandbox)
{
    const char *wayland = getenv("WAYLAND_DISPLAY");
    if (wayland == NULL || wayland[0] == '\0') return 0;
    char source[PATH_MAX];
    const char *name = strrchr(wayland, '/');
    name = name == NULL ? wayland : name + 1;
    if (copy_path(sandbox->wayland_name, sizeof(sandbox->wayland_name), name, "Wayland socket name") != 0) return 1;
    if (wayland[0] == '/')
    {
        if (copy_path(source, sizeof(source), wayland, "Wayland socket") != 0) return 1;
    }
    else if (hf_join(source, sizeof(source), sandbox->host_runtime, wayland) != 0)
        return hf_fail("the Wayland socket path");
    if (resolve_runtime_source(sandbox, "the Wayland socket", source, true,
        sandbox->wayland_socket, sizeof(sandbox->wayland_socket)) != 0) return 1;
    if (sandbox->wayland_socket[0] == '\0') sandbox->wayland_name[0] = '\0';
    return 0;
}

static int discover_pipewire_socket(struct hf_sandbox *sandbox)
{
    char source[PATH_MAX];
    if (hf_join(source, sizeof(source), sandbox->host_runtime, "pipewire-0") != 0)
        return hf_fail("the PipeWire socket path");
    return resolve_runtime_source(sandbox, "the PipeWire socket", source, true,
        sandbox->pipewire_socket, sizeof(sandbox->pipewire_socket));
}

static int discover_pulse_socket(struct hf_sandbox *sandbox)
{
    char source[PATH_MAX];
    if (hf_join(source, sizeof(source), sandbox->host_runtime, "pulse/native") != 0)
        return hf_fail("the PulseAudio socket path");
    return resolve_runtime_source(sandbox, "the PulseAudio socket", source, true,
        sandbox->pulse_socket, sizeof(sandbox->pulse_socket));
}

static int discover_display_sockets(struct hf_sandbox *sandbox)
{
    if (discover_wayland_socket(sandbox) != 0) return 1;
    if (discover_pipewire_socket(sandbox) != 0) return 1;
    return discover_pulse_socket(sandbox);
}

static int discover_x11_socket(struct hf_sandbox *sandbox)
{
    const char *display = getenv("DISPLAY");
    if (display == NULL || display[0] != ':') return 0;
    int number = 0;
    if (sscanf(display + 1, "%d", &number) != 1) return 0;
    char source[PATH_MAX];
    if (snprintf(source, sizeof(source), "/tmp/.X11-unix/X%d", number) >= (int)sizeof(source))
        return hf_fail_msg("the X11 socket path is too long");
    if (snprintf(sandbox->x11_name, sizeof(sandbox->x11_name), "X%d", number) >= (int)sizeof(sandbox->x11_name))
        return hf_fail_msg("the X11 socket name is too long");
    if (resolve_runtime_source(sandbox, "the X11 socket", source, true,
        sandbox->x11_socket, sizeof(sandbox->x11_socket)) != 0) return 1;
    if (sandbox->x11_socket[0] == '\0') sandbox->x11_name[0] = '\0';
    return 0;
}

static int discover_pulse_cookie(struct hf_sandbox *sandbox)
{
    const char *home = getenv("HOME");
    const char *config = getenv("XDG_CONFIG_HOME");
    char candidate[PATH_MAX];
    if (config != NULL && config[0] != '\0')
    {
        if (hf_join3(candidate, sizeof(candidate), config, "pulse", "cookie") != 0)
            return hf_fail("the PulseAudio cookie path");
    }
    else if (home != NULL && home[0] != '\0')
    {
        if (hf_join3(candidate, sizeof(candidate), home, ".config/pulse", "cookie") != 0)
            return hf_fail("the PulseAudio cookie path");
    }
    else return 0;
    return resolve_runtime_source(sandbox, "the PulseAudio cookie", candidate, false,
        sandbox->pulse_cookie, sizeof(sandbox->pulse_cookie));
}

static int discover_authority(struct hf_sandbox *sandbox)
{
    const char *authority = getenv("XAUTHORITY");
    if (authority != NULL && authority[0] != '\0')
        return resolve_runtime_source(sandbox, "the Xauthority file", authority, false,
            sandbox->xauthority, sizeof(sandbox->xauthority));
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') return 0;
    char candidate[PATH_MAX];
    if (hf_join(candidate, sizeof(candidate), home, ".Xauthority") != 0) return hf_fail("the Xauthority path");
    return resolve_runtime_source(sandbox, "the Xauthority file", candidate, false,
        sandbox->xauthority, sizeof(sandbox->xauthority));
}

int hf_discover_runtime_sockets(struct hf_sandbox *sandbox)
{
    sandbox->wayland_socket[0] = '\0';
    sandbox->wayland_name[0] = '\0';
    sandbox->pipewire_socket[0] = '\0';
    sandbox->pulse_socket[0] = '\0';
    sandbox->pulse_cookie[0] = '\0';
    sandbox->xauthority[0] = '\0';
    sandbox->x11_socket[0] = '\0';
    sandbox->x11_name[0] = '\0';
    if (sandbox->host_runtime[0] == '\0') return 0;
    if (discover_display_sockets(sandbox) != 0) return 1;
    if (discover_x11_socket(sandbox) != 0) return 1;
    if (discover_pulse_cookie(sandbox) != 0) return 1;
    return discover_authority(sandbox);
}

// An optional folder (Steam, the shader cache): resolve it when it exists, clear it when it does not. A path too long
// for this program's fixed buffers is still an error; a missing cache is not.
static int normalize_optional_dir(char *value, size_t size, const char *what)
{
    if (value[0] == '\0' || !is_directory(value)) { value[0] = '\0'; return 0; }
    char *resolved = realpath(value, NULL);
    if (resolved == NULL) { value[0] = '\0'; return 0; }
    int result = copy_path(value, size, resolved, what);
    free(resolved);
    return result;
}

// The only writable path outside gameOut is Steam's per-game shader cache. Do not let an environment variable turn that
// reviewed exception into a writable Steam library or home folder: the resolved path must be the game's own cache.
static bool is_game_shader_cache(const char *path)
{
    static const char suffix[] = "/steamapps/shadercache/553850";
    size_t length = strlen(path), tail = sizeof(suffix) - 1;
    return length >= tail && strcmp(path + length - tail, suffix) == 0;
}

static int discover_steam(struct hf_sandbox *sandbox)
{
    if (env_path("STEAM_COMPAT_CLIENT_INSTALL_PATH", sandbox->steam_root, sizeof(sandbox->steam_root)) != 0) return 1;
    if (normalize_optional_dir(sandbox->steam_root, sizeof(sandbox->steam_root), "Steam") != 0) return 1;
    const char *shader = getenv("STEAM_COMPAT_SHADER_PATH");
    if (shader != NULL && shader[0] != '\0')
    {
        if (copy_path(sandbox->shader_cache, sizeof(sandbox->shader_cache), shader, "shader cache") != 0) return 1;
    }
    else if (sandbox->steam_root[0] != '\0' &&
        hf_join3(sandbox->shader_cache, sizeof(sandbox->shader_cache), sandbox->steam_root, "steamapps/shadercache", "553850") != 0)
        return hf_fail("the shader cache path");
    if (normalize_optional_dir(sandbox->shader_cache, sizeof(sandbox->shader_cache), "shader cache") != 0) return 1;
    if (sandbox->shader_cache[0] != '\0' && !is_game_shader_cache(sandbox->shader_cache))
        return hf_fail_msg("STEAM_COMPAT_SHADER_PATH is not this game's shader cache (.../steamapps/shadercache/553850): %s",
            sandbox->shader_cache);
    return 0;
}

// hf-inner.sh puts Steam's Fossilize (shader recording, G.1) and overlay manifests in the host's
// ${XDG_DATA_HOME:-$HOME/.local/share}/vulkan/implicit_layer.d. Redirecting XDG_DATA_HOME would hide them, so bind that
// folder read-only at the same place under the session's data home.
static int discover_vulkan_layers(struct hf_sandbox *sandbox)
{
    const char *data = getenv("XDG_DATA_HOME");
    char base[PATH_MAX];
    if (data != NULL && data[0] != '\0')
    {
        if (copy_path(base, sizeof(base), data, "XDG_DATA_HOME") != 0) return 1;
    }
    else if (sandbox->host_home[0] != '\0')
    {
        if (hf_join(base, sizeof(base), sandbox->host_home, ".local/share") != 0) return hf_fail("the data home path");
    }
    else return 0;
    char layers[PATH_MAX];
    if (hf_join3(layers, sizeof(layers), base, "vulkan", "implicit_layer.d") != 0) return hf_fail("the Vulkan layer path");
    if (!is_directory(layers)) return 0;
    // Resolve before the view so the bind source is stable even when XDG_DATA_HOME is a link.
    char *resolved = realpath(layers, NULL);
    if (resolved == NULL) return hf_fail(layers);
    int result = copy_path(sandbox->vulkan_layers, sizeof(sandbox->vulkan_layers), resolved, "Vulkan implicit layers");
    free(resolved);
    return result;
}

static int proton_fixup_paths(const struct hf_sandbox *sandbox, char *json, size_t json_size,
    char *stamp, size_t stamp_size)
{
    if (hf_join(json, json_size, sandbox->proton_root, "steampipe_fixups.json") != 0 ||
        hf_join(stamp, stamp_size, sandbox->proton_root, "files/steampipe_fixups_mtime") != 0)
        return hf_fail("the Proton fixup paths");
    return 0;
}

// 0: read (or no steampipe_fixups.json, has_json=false); 1: fail.
static int read_fixup_state(const char *json, const char *stamp, struct stat *info, char *text, size_t size,
    bool *has_json)
{
    *has_json = false;
    if (stat(json, info) != 0)
    {
        if (errno == ENOENT) return 0;
        return hf_fail(json);
    }
    *has_json = true;
    if (!S_ISREG(info->st_mode))
        return hf_fail_msg("Proton's steampipe_fixups.json is not a regular file: %s", json);
    struct stat stamp_info;
    if (lstat(stamp, &stamp_info) != 0 || !S_ISREG(stamp_info.st_mode))
        return hf_fail_msg("Proton needs to write its steampipe fixups (files/steampipe_fixups_mtime is missing or not "
            "a regular file); run Proton once outside the sandbox so it can update its own folder: %s", stamp);
    if (read_text(stamp, text, size) != 0)
        return hf_fail_msg("Proton needs to write its steampipe fixups (files/steampipe_fixups_mtime is unreadable); "
            "run Proton once outside the sandbox so it can update its own folder: %s", stamp);
    return 0;
}

static int validate_fixup_timestamp(const struct stat *info, const char *text, const char *stamp)
{
    char *end = NULL;
    long double recorded = strtold(text, &end);
    bool parsed = end != text;
    while (end != NULL && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) end++;
    if (!parsed || (end != NULL && *end != '\0'))
        return hf_fail_msg("Proton's steampipe fixup timestamp is invalid; run Proton once outside the sandbox: %s", stamp);
    long double actual = (long double)info->st_mtim.tv_sec + (long double)info->st_mtim.tv_nsec / 1000000000.0L;
    long double difference = recorded - actual;
    if (difference < 0) difference = -difference;
    if (difference > 0.000001L)
        return hf_fail_msg("Proton's steampipe fixups are stale (files/steampipe_fixups_mtime does not match "
            "steampipe_fixups.json). Run Proton once outside the sandbox so it can update its own folder, then retry.");
    return 0;
}

static int proton_fixups(struct hf_sandbox *sandbox)
{
    char json[PATH_MAX], stamp[PATH_MAX], text[128];
    if (proton_fixup_paths(sandbox, json, sizeof(json), stamp, sizeof(stamp)) != 0) return 1;
    struct stat info;
    bool has_json;
    if (read_fixup_state(json, stamp, &info, text, sizeof(text), &has_json) != 0) return 1;
    if (!has_json) return 0;
    return validate_fixup_timestamp(&info, text, stamp);
}

// Proton opens <proton>/dist.lock O_RDWR|O_CREAT|O_TRUNC before anything else. It is the only file in the Proton folder
// made writable in the view. Refuse links and special files; create an empty regular file when it is missing, in the
// trusted setup before the view exists.
static int proton_lock(struct hf_sandbox *sandbox)
{
    if (sandbox->proton_root[0] == '\0') return 0;
    if (proton_fixups(sandbox) != 0) return 1;
    if (hf_join(sandbox->proton_lock, sizeof(sandbox->proton_lock), sandbox->proton_root, "dist.lock") != 0)
        return hf_fail("Proton's dist.lock path");
    struct stat info;
    if (lstat(sandbox->proton_lock, &info) == 0)
    {
        if (!S_ISREG(info.st_mode)) return hf_fail_msg("Proton's dist.lock is not a regular file: %s", sandbox->proton_lock);
    }
    else if (errno == ENOENT)
    {
        int fd = open(sandbox->proton_lock, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) return hf_fail("creating Proton's dist.lock");
        close(fd);
        if (lstat(sandbox->proton_lock, &info) != 0 || !S_ISREG(info.st_mode))
            return hf_fail_msg("Proton's dist.lock is not a regular file after creation: %s", sandbox->proton_lock);
    }
    else return hf_fail("reading Proton's dist.lock");
    if (access(sandbox->proton_lock, W_OK) != 0)
        return hf_fail_msg("Proton's dist.lock is not writable by the player: %s", sandbox->proton_lock);
    return 0;
}

static int proton_from_env(struct hf_sandbox *sandbox, const char *proton)
{
    char resolved[PATH_MAX];
    struct stat info;
    if (resolve_existing(proton, resolved, sizeof(resolved), "PROTON") != 0) return 1;
    if (stat(resolved, &info) == 0 && S_ISDIR(info.st_mode))
        return copy_path(sandbox->proton_root, sizeof(sandbox->proton_root), resolved, "Proton");
    char *slash = strrchr(resolved, '/');
    if (slash == NULL || slash == resolved) return hf_fail_msg("the Proton script has no folder");
    *slash = '\0';
    return copy_path(sandbox->proton_root, sizeof(sandbox->proton_root), resolved, "Proton");
}

// 0: not a Proton command, 1: proton_root filled, -1: a path failed.
static int proton_from_command(struct hf_sandbox *sandbox)
{
    const char *name = strrchr(sandbox->command_path, '/');
    if (name == NULL || strcmp(name + 1, "proton") != 0) return 0;
    if (copy_path(sandbox->proton_root, sizeof(sandbox->proton_root), sandbox->command_path, "Proton") != 0) return -1;
    char *slash = strrchr(sandbox->proton_root, '/');
    if (slash == NULL || slash == sandbox->proton_root)
    {
        hf_fail_msg("the Proton path has no folder");
        return -1;
    }
    *slash = '\0';
    return 1;
}

static int discover_proton(struct hf_sandbox *sandbox)
{
    const char *proton = getenv("PROTON");
    if (proton != NULL && proton[0] != '\0')
    {
        if (proton_from_env(sandbox, proton) != 0) return 1;
    }
    else
    {
        // A non-Proton command (/bin/true in a test) has no Proton root; that is not an error.
        int result = proton_from_command(sandbox);
        if (result < 0) return 1;
        if (result == 0) return 0;
    }
    return proton_lock(sandbox);
}

int hf_discover_environment(struct hf_sandbox *sandbox, char **argv, int argc)
{
    (void)argv;
    (void)argc;
    if (discover_host_home(sandbox) != 0) return 1;
    if (discover_steam(sandbox) != 0) return 1;
    if (discover_vulkan_layers(sandbox) != 0) return 1;
    if (discover_proton(sandbox) != 0) return 1;
    return hf_discover_runtime_sockets(sandbox);
}

static int resolve_in_path(const char *command, char *out, size_t size)
{
    const char *path = getenv("PATH");
    if (path == NULL || path[0] == '\0') return hf_fail_msg("PATH is empty and the command is not an absolute path");
    char copy[PATH_MAX];
    if (copy_path(copy, sizeof(copy), path, "PATH") != 0) return 1;
    char *save = NULL;
    for (char *directory = strtok_r(copy, ":", &save); directory != NULL; directory = strtok_r(NULL, ":", &save))
    {
        char candidate[PATH_MAX];
        const char *folder = directory[0] == '\0' ? "." : directory;
        if (hf_join(candidate, sizeof(candidate), folder, command) != 0) continue;
        if (access(candidate, X_OK) != 0) continue;
        if (resolve_existing(candidate, out, size, "command") != 0) return 1;
        return 0;
    }
    return hf_fail_msg("the command was not found on PATH: %s", command);
}

int hf_resolve_command(struct hf_sandbox *sandbox, const char *command)
{
    if (strchr(command, '/') != NULL) return resolve_existing(command, sandbox->command_path, sizeof(sandbox->command_path), "command");
    // "proton" in the wrapper means $PROTON (hf-inner.sh runs "$PROTON run ..."). Use the same script, not whatever a
    // PATH entry happens to name.
    const char *proton = getenv("PROTON");
    if (strcmp(command, "proton") == 0 && proton != NULL && proton[0] != '\0')
        return resolve_existing(proton, sandbox->command_path, sizeof(sandbox->command_path), "PROTON");
    return resolve_in_path(command, sandbox->command_path, sizeof(sandbox->command_path));
}
