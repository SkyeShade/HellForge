# Hooks and game memory

This chapter is for code mod authors who need something the SDK's fields, events and actions do not cover yet. For
that, a code mod has the same reach HellForge's engine has: it can find any function of the game, hook it (run your code
when the game calls it, with the original a call away), read and write any memory, and patch the game's code.

After reading it you will be able to:

- decide when a hook is the right tool, and when the SDK already does the job;
- find a game function and check it is the one you studied before touching it;
- hook it with a detour that calls the original, and share it with other mods' hooks;
- patch a few bytes of the game's code, and read and write game memory safely;
- keep a hook correct across threads and in co-op, and find out why one failed.

Before you start, you should know C pointers and function pointers, and have a disassembler or debugger to study the
game's code with. Read [logic at run time](runtime.md) first: its events and actions are the checked way to do much of
what people reach for hooks for, and its [game thread](runtime.md#the-game-thread) section matters here too.

## Contents

- [When to hook](#when-to-hook)
- [How a hook works](#how-a-hook-works)
- [The calls](#the-calls)
- [Finding a function](#finding-a-function)
- [Hooking it](#hooking-it)
- [Writing a detour](#writing-a-detour)
- [Threads](#threads)
- [Several mods, one function](#several-mods-one-function)
- [Patching code](#patching-code)
- [Reading and writing data](#reading-and-writing-data)
- [Co-op](#co-op)
- [When it goes wrong](#when-it-goes-wrong)
- [From a hook to an event](#from-a-hook-to-an-event)
- [Where to go next](#where-to-go-next)

## When to hook

Use the SDK where it reaches, and hooks where it does not:

| You want to | Use |
|---|---|
| change a value the game has (a weapon's rate, a stratagem's cooldown) | [a definition block or a live edit](content.md) |
| add a weapon, stratagem, entity, projectile... | [content](content.md) |
| know when something happens | [a game event](runtime.md#game-events), if there is one |
| make the game do something it does (heal, explode, push) | [an action](runtime.md#actions), if there is one |
| anything else: a function with no event, a rule in code, a value no mapping names | hooks, code patches, memory |

The SDK's ways are checked against the game build, reported when they conflict, and kept working as HellForge grows.
A hook is yours to get right, and is as powerful as you make it.

## How a hook works

A hook redirects a function of the game to a function of yours, the detour. When the game calls the hooked function,
your detour runs instead, with the same arguments. To run the game's own code, the detour calls the original, a pointer
HellForge gives you. In the end it runs the function's first instructions (moved aside when the hook was placed) and
then the rest of the function. Your detour can change the arguments before that call, change the result after it, or skip the
call and replace the function.

HellForge keeps one chain per hooked function. The function's start jumps to the first detour; each detour's original
leads to the next detour in the chain, and the last one's leads to the game's code. A hook from your mod joins the
chain, so several mods (and HellForge itself) can hook the same function without replacing each other
([several mods, one function](#several-mods-one-function)).

## The calls

| Call | Does |
|---|---|
| `module_base("game.dll", &size)` | a module's base address and image size (`"game.dll"`, `"helldivers2.exe"`, any loaded DLL), or 0 |
| `find_pattern("game.dll", "48 8B ?? 24 ?? E8")` | the first match of an IDA-style byte pattern in a module's image (`??` any byte), or 0 |
| `read(address, buffer, size)` | copies memory; 0 on success, 1 for an address that cannot be read (no crash) |
| `write(address, data, size)` | writes code or data: makes the bytes writable, restores their protection, flushes the instruction cache |
| `write_u32_if(mod, address, expect, value)` | writes a u32 only if it holds `expect` (or already `value`); logged |
| `hook(mod, target, detour, &original)` | detours the function at `target` to yours; `original` calls the next hook, or the game's code. 0 on success |
| `hook_ex(mod, target, detour, &original, priority)` | the same with a priority ([several mods, one function](#several-mods-one-function)); SDK 13 |
| `code_matches(address, pattern)` | 1 when the code at `address`, as it was before any hook HellForge placed, matches the pattern; SDK 13 |
| `read_code(address, buffer, size)` | like `read`, with a hooked function's first bytes as they were before HellForge's hooks; SDK 13 |

The others exist since SDK 1. Hooks stay for the session.

## Finding a function

HellForge runs one build of the game (25480438) on every machine, so an address found once stays right for as long as
HellForge supports that build: a function is `module_base("game.dll", NULL) + its offset` in every session. To find
offsets, use HD2Runtime's research notes, the comments of the examples (`vanilla-plus` documents every address it
touches), and your own reverse-engineering tools.

### Check before you touch

Confirm the bytes at the address are the ones you studied, so a mistake (or someone running another build) is refused
instead of crashing:

```c
#define TARGET 0x123450u   /* placeholder: your function's offset in game.dll 25480438 */
static const char kTarget[] = "48 89 5C 24 ?? 57 48 83 EC 20";  /* placeholder: its first bytes, as you studied them */

uintptr_t game = hf->module_base("game.dll", NULL);
if (game == 0 || !hf->code_matches(game + TARGET, kTarget))
{
    hf->log(mod, "the function is not the one this mod knows; nothing hooked");
    return;
}
```

Use `??` for bytes that hold addresses. Which check to use depends on what you are about to do:

- Before hooking, use `code_matches`. It reads a hooked function's first 32 bytes as they were before the hooks
  HellForge placed (its own and every mod's), so a function another mod or HellForge has hooked already still matches,
  and your hook joins the chain. Changes made by `write`, by HellForge's own code rewrites or by other tools read as
  they are now.
- Before patching bytes, use `find_pattern` or `read`. They read the code as it is now: if a hook or another mod's patch
  changed those bytes, your check fails, which is what you want before overwriting them.
  `find_pattern(module, pattern) == game + TARGET` also proves the pattern is unique up to that point (`vanilla-plus`
  does this).

## Hooking it

Hook from `HF_EVENT_GAME_LOADED` (before it, game.dll's code is not ready) or later. The pattern, with a placeholder
target and signature you replace with the function you studied:

```c
/* The function's real signature: every parameter, in order, with its real type. */
typedef int64_t (*TargetFn)(void* self, float amount, uint32_t flags);   /* placeholder signature */
static TargetFn Original;   /* calls the game's code */
static const HfApi* hf;

static int64_t Detour(void* self, float amount, uint32_t flags)
{
    int64_t result = Original(self, amount, flags);   /* the game's code */
    /* your change: after (as here), before, or instead of it */
    return result;
}

static void GameLoaded(HfMod* mod, HfEvent event, void* user)
{
    uintptr_t game = hf->module_base("game.dll", NULL);
    if (game == 0 || !hf->code_matches(game + TARGET, kTarget)) { hf->log(mod, "not hooked"); return; }
    if (hf->hook(mod, game + TARGET, (void*)Detour, (void**)&Original) != 0) return;  /* the log says why */
    hf->log(mod, "hooked");
}

HF_MOD_EXPORT int hellforge_mod_init(const HfApi* api, HfMod* mod)
{
    if (api->version < 13) return 1;   /* code_matches; the manifest asks for "sdk": 13 */
    hf = api;
    api->on(mod, HF_EVENT_GAME_LOADED, GameLoaded, NULL);
    return 0;
}
```

The loader's own test mod (`native/mod-loader/test/hook_test_mod.c`) hooks a stand-in function this way against a fake
game. Its detour is the smallest complete one: it calls the original and adds 1000 to what it returns.

```c
typedef int (*Target)(int);
static Target original;

static int Detour(int x) { return original(x) + 1000; }

/* at HF_EVENT_GAME_LOADED */
if (hellforge->hook(mod, target, (void*)Detour, (void**)&original) == 0) hellforge->log(mod, "detour installed");
```

`hook` writes `hooked game.dll+0x... (priority 0; 1 of 1 in its chain)` in your log, or why it could not: the
function's first bytes, and what was tried. HellForge hooks with MinHook. Where there is no memory near the function
(code behind an overlay's stub, for one), it takes over the overlay's stub or uses a far jump, and says so.

## Writing a detour

- Match the signature exactly. Windows x64 has one calling convention: the first four parameters in `rcx`, `rdx`,
  `r8`, `r9` (`xmm0` to `xmm3` for floats), the rest on the stack, the result in `rax` (`xmm0` for a float). A detour
  declared with the wrong types or too few parameters passes the original garbage. Unsure of a parameter's type? Use
  the right size (`uint64_t`, `void*`) for integers and pointers, and `float` / `double` where the game passes floats.
- Call the original unless you mean to replace the function, with the arguments it expects, and return what it
  returns. Change arguments before the call, or the result after it.
- Be quick. Many functions run thousands of times a frame. No file writes, no waiting, no locks the game might also
  want. Log once, or count and log a summary.
- Expect re-entry. The original may call itself, or another function you hooked.
- Do not let anything escape. A C++ exception or a fault in a detour ends the game.

## Threads

A detour runs on the thread that called the function, which is often one of the game's worker threads, not the game
thread. So:

- Keep shared state safe across threads (atomics, or a lock you never hold while calling into the game).
- [Actions](runtime.md#actions) from a detour off the game thread wait for the next game update (`HF_ACT_QUEUED`). That
  is fine for most, but do not wait on them.
- `hf->log` can be called from any thread (one write per line). It is a file write, so do not call it on every call of
  a busy function.
- For work that must happen on the game thread, note it in the detour and do it in an
  [`on_game_thread`](runtime.md#the-game-thread) callback.

## Several mods, one function

Hooks chain: several mods can hook the same function, and so can HellForge, which hooks some of the game's functions for
its own events and registries (up to 16 hooks per function in all). Nobody replaces anybody. When the game calls the
function, the hooks run in this order, each calling the next through its `original`:

1. Mods' hooks first, highest priority first. `hook` is priority 0; `hook_ex(mod, target, detour, &original,
   priority)` (SDK 13) sets one.
2. Equal priorities in load order: the mod higher on the Mods page (loaded later) runs first, as it wins other
   conflicts. One mod hooking a function twice: its later hook runs first.
3. HellForge's own hooks last, nearest the game's code. What your detour passes on is what HellForge's events see: a
   mod that changes a hit's damage before calling `original` changes what `HF_ON_DAMAGE` reports.
4. The game's code at the end.

```c
/* run before other mods' hooks on this function (higher first) */
if (hf->size >= offsetof(HfApi, hook_ex) + sizeof hf->hook_ex)
    hf->hook_ex(mod, game + TARGET, (void*)Detour, (void**)&Original, 10);
else
    hf->hook(mod, game + TARGET, (void*)Detour, (void**)&Original);
```

### Rules for chained hooks

- Always call through the variable you passed as `original`, never a copy of it: HellForge updates it when a hook
  joins the chain after yours.
- Hook from `HF_EVENT_GAME_LOADED` on. The first hook on a function patches its code; later ones only rewrite pointers,
  so they are safe while the game runs. Hooks stay for the session; there is no unhook.
- Check with `code_matches`, which sees a function as it was before HellForge's hooks: a function someone hooked first
  still matches. `find_pattern` sees the hook's jump instead ([finding a function](#finding-a-function)). HellForge's
  own checks work the same way, so your hook never stops HellForge's events from installing.
- The same detour on the same function twice is refused; so is a function whose code cannot be hooked (the log shows
  its first bytes). There is room for 16 hooks per function and 1,024 hooked functions, HellForge's own included.
- HellForge rewrites some of the game's instructions itself, most of them while the game starts, before
  `HF_EVENT_GAME_LOADED` ([patching code](#patching-code)). If one is in the bytes you check, your check fails cleanly.
  A rewrite HellForge would make later over a hook is refused and logged, not patched over; the feature it was for then
  stays off.
- HellForge's [actions](runtime.md#actions) call game functions too (an explosion through the game's own request, say).
  A detour on such a function also runs when any mod uses the action.

### Who hooks what

Your log says `hooked game.dll+0x... (priority 0; 1 of 2 in its chain)`. When a function gets a second owner,
`modloader.log` says `hooks: shared hook game.dll+0x..., first to last: a (priority 5), then b (priority 0), then
HellForge (the damage consumer) (priority 0)`, and each owner's log has the same line without `hooks: `. Each session's
`hooks.tsv` lists every hooked function with its owners in order.

## Patching code

Some changes are a few bytes of the game's code: a branch taken or skipped, a constant, a call sent elsewhere.
`examples/mods/vanilla-plus` does two kinds:

- A branch. Its hellpod steering fix changes a two-byte `jp` into a `jmp` past the code it skips: checked with
  `find_pattern` at the exact offset, then `write`.
- A jump to a stub. Its reinforcement fix replaces 23 bytes at a call site with a 14-byte absolute jump
  (`FF 25 00000000` and the address) to a stub of its own, which repeats the replaced instructions, does its own test,
  and jumps back. The stub lives in memory the mod allocates (`VirtualAlloc`, then made executable).

The steering fix is the whole pattern in a few lines: check the bytes at the exact offset, then write.

```c
#define STEERING_SITE 0x92E0A1u
#define STEERING_BRANCH 0x2Fu
static const char kSteeringSite[] = "49 8B 47 60 48 8B 55 F8 F3 0F 10 25 3F DF A9 01 F3 0F 10 4D 50 41 0F 2E CE F3 0F 10 14 D0 F3 0F 10 5C D0 04 "
                                    "0F 57 D4 F3 0F 10 45 54 0F 57 DC 7A 0A 75 08 41 0F 2E C6 7A 02 74 0C";

static void SteerAnywhere(HfMod* mod, HfEvent event, void* user)
{
    uintptr_t game = hellforge->module_base("game.dll", NULL);
    if (game == 0 || hellforge->find_pattern("game.dll", kSteeringSite) != game + STEERING_SITE)
    {
        hellforge->log(mod, "hellpod steering: the steering code is not the reviewed one (another game build?); left as it is");
        return;
    }
    static const uint8_t jump[] = {0xEB, 0x16};
    if (hellforge->write(game + STEERING_SITE + STEERING_BRANCH, jump, sizeof jump) != 0)
        hellforge->log(mod, "hellpod steering: the steering code could not be changed; left as it is");
    else
        hellforge->log(mod, "hellpod steering: your hellpod follows your steering over any ground (the keep-out push is skipped)");
}
```

### Rules for patches

- Patch from `HF_EVENT_GAME_LOADED`, while the game is still starting, and replace whole instructions. A patch that
  cuts one in half, or one made in code another thread is running at that moment, crashes.
- A patch is not a hook: two mods patching the same bytes break each other, and nothing reports it. Where a hook does
  the job, prefer the hook: hooks on one function [chain](#several-mods-one-function).
- HellForge itself rewrites some of the game's code (where it moves the game's lists of projectiles, damage, stratagems
  and difficulties, and where its events read the game). Check the bytes you patch are the build's own, as above: if
  HellForge or another mod changed them first, your check fails cleanly.

## Reading and writing data

- Game memory beyond the code (objects, lists, allocations) moves from session to session. Find it every session from
  something stable (a global the code points to, a record by its entity), never from an address you saved.
- `read` is safe on any address: it fails instead of crashing. Read through it when you are not sure a pointer is good.
- `write_u32_if` writes a value only over the one you expect, and logs both.
- Records and settings rows have named fields and checked calls ([content](content.md),
  [live edits](runtime.md#live-edits)). Writing them by hand skips the conflict reports and the checks.

## Co-op

- Hooks and patches run on every machine that has your mod, and friends who play together have the same gameplay mods
  (the modlist code makes sure). Keep your mod's choices the same on every machine: decide from the game's data, not
  from the local clock or a random number of your own.
- Some things only the host decides (where a reinforcement lands, enemy spawns, the mission timer). A change on a joiner
  does nothing there, or makes its game disagree with the host's. `hf->session(mod, &out)` says which you are.
- A hit is applied by the machine that owns the target. A damage change in a hook changes the hits that machine applies.

## When it goes wrong

- Your log (`logs/sessions/<session>/mod-<id>.log`) has every hook and why one failed, with the function's first
  bytes.
- A crash leaves its note in `modloader.log`: where it happened as `module+offset` (`game.dll+0x...`, or your DLL's
  name), what it read, the registers and the game addresses on the stack, and a crash dump beside the logs to open in a
  debugger (a small one, or a full memory dump with the launcher's Crash dump setting). See
  [debugging](debugging.md#crashes).
- Test alone first, with only your mod on, then with the mods yours will run beside.

## From a hook to an event

Every event and action in the SDK started as someone's hook. If yours answers a common need (something happened, make
the game do something), say which function you hooked and why: it can become an event or an action every mod shares,
checked and kept working.

## Where to go next

Next in the guide: [debugging](debugging.md), for the logs, the reasons HellForge gives for refusing something, and
crashes.

In the [SDK reference](../sdk-reference.md): `module_base`, `find_pattern`, `read`, `write`, `write_u32_if` and `hook`
under [SDK 1](../sdk-reference.md#sdk-1), and `hook_ex`, `code_matches` and `read_code` under
[SDK 13](../sdk-reference.md#sdk-13).
