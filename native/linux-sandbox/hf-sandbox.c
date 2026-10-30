// hf-sandbox: the phase-2 Wine-domain filesystem sandbox. See hf-sandbox.h and .deepcode/review-F4b.md.
//
// Usage:
//   hf-sandbox --session <run dir> --prefix <real pfx> [--steam-runtime <dir>]... [--bind-ro <path>]... -- <command...>
//   hf-sandbox --self-test
//
// The first form builds the game domain and execs the command in it. --bind-ro exposes one existing file or folder
// read-only at the same path; the native supervisor derives each one from the validated session request. --steam-runtime
// names a Steam tool folder that the Proton's own toolmanifest requires (the supervisor resolves it; never the request).
// The second form
// builds the same domain from a temporary tree, applies the same guard and runs the G1 canaries. Both fail closed: a
// failed step prints one reason and returns non-zero; the command never runs unsandboxed.

#include "hf-sandbox.h"

struct hf_cli
{
    const char *session;
    const char *prefix;
    const char *self_test_dir;
    char *bind_ro[HF_MAX_EXTRA_BINDS];
    int bind_ro_count;
    char *steam_runtime[HF_MAX_STEAM_RUNTIMES];
    int steam_runtime_count;
    char **command;
    int command_count;
    bool self_test;
};

static void usage(const char *program)
{
    fprintf(stderr,
        "Usage: %s --session <run dir> --prefix <real pfx> [--steam-runtime <dir>]... [--bind-ro <path>]... -- <command...>\n"
        "       %s --self-test [--self-test-dir <folder>]\n"
        "\n"
        "--session is a run folder below <installation>/logs/sessions. --prefix is the Steam compatdata folder that holds\n"
        "pfx/ (STEAM_COMPAT_DATA_PATH), not the Wine prefix itself. --bind-ro exposes an existing link-free file or folder\n"
        "read-only at the same path; it must be below mods/ or temp/mod-builds/, or be the exact app/session folder or mod\n"
        "loader. A bind covered by an earlier bind is skipped. --steam-runtime exposes a Steam tool folder read-only; it may\n"
        "not be / or cover the home, and may not be inside or cover the installation, the prefix or the session.\n"
        "The command must be an exposed system or Proton program.\n"
        "--session, --prefix and a command containing / are resolved against the caller's current folder; the command runs\n"
        "with its working directory set to the session's\n"
        "game folder. --self-test-dir defaults to the current folder and must not be under /tmp, /dev/shm, /run or /usr.\n",
        program, program);
}

// A repeated option's value (--bind-ro, --steam-runtime), up to its limit. 0: added, 2: too many.
static int add_repeated(char **list, int *count, int limit, char **argv, int *index)
{
    if (*count >= limit) { usage(argv[0]); return 2; }
    list[(*count)++] = argv[++*index];
    return 0;
}

// 0: handled, 1: help printed, 2: bad argument.
static int parse_option(int argc, char **argv, int *index, struct hf_cli *cli)
{
    const char *option = argv[*index];
    if (strcmp(option, "--self-test") == 0) { cli->self_test = true; return 0; }
    if (strcmp(option, "--self-test-dir") == 0 && *index + 1 < argc) { cli->self_test_dir = argv[++*index]; return 0; }
    if (strcmp(option, "--session") == 0 && *index + 1 < argc) { cli->session = argv[++*index]; return 0; }
    if (strcmp(option, "--prefix") == 0 && *index + 1 < argc) { cli->prefix = argv[++*index]; return 0; }
    if (strcmp(option, "--bind-ro") == 0 && *index + 1 < argc)
        return add_repeated(cli->bind_ro, &cli->bind_ro_count, HF_MAX_EXTRA_BINDS, argv, index);
    if (strcmp(option, "--steam-runtime") == 0 && *index + 1 < argc)
        return add_repeated(cli->steam_runtime, &cli->steam_runtime_count, HF_MAX_STEAM_RUNTIMES, argv, index);
    if (strcmp(option, "--help") == 0 || strcmp(option, "-h") == 0) { usage(argv[0]); return 1; }
    usage(argv[0]);
    return 2;
}

static int validate_cli(char **argv, const struct hf_cli *cli)
{
    if (cli->self_test)
    {
        if (cli->session != NULL || cli->prefix != NULL || cli->bind_ro_count > 0 || cli->steam_runtime_count > 0 ||
            cli->command_count > 0) { usage(argv[0]); return 2; }
        return 0;
    }
    if (cli->self_test_dir != NULL) { usage(argv[0]); return 2; }
    if (cli->session == NULL || cli->prefix == NULL || cli->command_count < 1) { usage(argv[0]); return 2; }
    return 0;
}

static void add_command(int argc, char **argv, int index, struct hf_cli *cli)
{
    (void)argc;
    if (cli->command == NULL) cli->command = &argv[index];
    cli->command_count++;
}

// 0: run, 1: help printed, 2: bad arguments.
static int parse_arguments(int argc, char **argv, struct hf_cli *cli)
{
    bool after_separator = false;
    for (int index = 1; index < argc; index++)
    {
        if (after_separator) { add_command(argc, argv, index, cli); continue; }
        if (strcmp(argv[index], "--") == 0) { after_separator = true; continue; }
        int status = parse_option(argc, argv, &index, cli);
        if (status != 0) return status;
    }
    return validate_cli(argv, cli);
}

static int prepare_run(struct hf_sandbox *sandbox)
{
    if (hf_prepare_paths(sandbox, sandbox->session, sandbox->prefix) != 0) return 1;
    if (hf_resolve_command(sandbox, sandbox->command[0]) != 0) return 1;
    if (hf_discover_environment(sandbox, sandbox->command, sandbox->command_count) != 0) return 1;
    if (hf_create_session_dirs(sandbox) != 0) return 1;
    if (hf_copy_lsfg_config(sandbox) != 0) return 1;
    return hf_prepare_session_home(sandbox);
}

static int build_run(struct hf_sandbox *sandbox)
{
    hf_note("F.4b phase 2: game domain for %s (prefix %s)", sandbox->game_out, sandbox->prefix);
    if (hf_enter_namespaces(sandbox) != 0) return 1;
    if (hf_mount_overlay(sandbox) != 0) return 1;
    if (hf_mask_overlay_install(sandbox) != 0) return 1;
    if (hf_build_root_view(sandbox) != 0) return 1;
    if (hf_pivot_into_root(sandbox) != 0) return 1;
    if (hf_apply_environment(sandbox) != 0) return 1;
    return hf_check_vulkan_layers(sandbox);
}

static int guard_and_exec(struct hf_sandbox *sandbox)
{
    if (hf_apply_landlock(sandbox) != 0) return 1;
    if (hf_apply_seccomp() != 0) return 1;
    if (hf_drop_capabilities() != 0) return 1;
    if (hf_verify_view(sandbox) != 0) return 1;
    if (hf_close_inherited_fds() != 0) return 1;
    return hf_exec_command(sandbox);
}

static int run_sandbox(struct hf_sandbox *sandbox)
{
    if (prepare_run(sandbox) != 0) return 1;
    if (build_run(sandbox) != 0) return 1;
    return guard_and_exec(sandbox);
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    struct hf_cli cli;
    memset(&cli, 0, sizeof(cli));
    int parsed = parse_arguments(argc, argv, &cli);
    if (parsed == 1) return 0;
    if (parsed != 0) return 2;
    if (cli.self_test) return hf_run_self_test(cli.self_test_dir);

    struct hf_sandbox sandbox;
    memset(&sandbox, 0, sizeof(sandbox));
    sandbox.command = cli.command;
    sandbox.command_count = cli.command_count;
    sandbox.extra_binds = cli.bind_ro;
    sandbox.extra_bind_count = cli.bind_ro_count;
    sandbox.steam_runtimes = cli.steam_runtime;
    sandbox.steam_runtime_count = cli.steam_runtime_count;
    // The paths are filled by hf_prepare_paths; hand it the raw arguments first.
    if (snprintf(sandbox.session, sizeof(sandbox.session), "%s", cli.session) >= (int)sizeof(sandbox.session) ||
        snprintf(sandbox.prefix, sizeof(sandbox.prefix), "%s", cli.prefix) >= (int)sizeof(sandbox.prefix))
        return hf_fail_msg("an argument path is too long");
    return run_sandbox(&sandbox);
}
