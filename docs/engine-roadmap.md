# HellForge engine: roadmap

This page is for contributors. It lists what is done and what is planned in the engine (the session runner, the local
backend, the mod loader and the stand-ins); [engine.md](engine.md) describes the engine as it is, and
[sdk-roadmap.md](sdk-roadmap.md) is the mod SDK's side.

Current versions: HellForge 0.3.0-beta, engine 0.1.40, mod SDK 15 (`Directory.Build.props`,
`native/mod-sdk/hellforge_mod.h`).

## Network isolation

Added 2026-10-18.

Status: the census (step 1, the game's view) runs in every session, and the WFP block (step 5) is what setup installs.
Windows' own view of the census, steps 2 to 4 and the near-term items are not done.

### Goal

The copied game never reaches the official servers, and HellForge can show it. Every connection the copy attempts is
known, and each one is either allowed (this PC's local backend, the Steam client) or caught. When this goal was set, the
guarantee rested on Windows Firewall alone. The aim is to know the copy's traffic well enough not to depend on it, and to
tell players exactly what is wrong when the block is not in force.

### The firewall path (where this started)

`scripts/Install-NetworkBlock.ps1` adds persistent Windows Firewall rules (once, elevated) for the copy's programs.
Every session checks them and runs `NetworkProbe.exe` (one of the blocked programs): the local backend must answer on
loopback, and a connection to 192.0.2.1:443 must be refused by the firewall (WSAEACCES, 10013), or the session stops
before the game starts. Installations without the WFP marker (see step 5) still take this path.

Seen in the beta (2026-10-18):

- For many players the administrator prompt never showed, and the block was missing until they ran HellForge as
  administrator. The session runner, which asks, has no window, and setup asks without its window as the prompt's
  owner.
- Security suites and VPNs whose own network filters let traffic past the rules (Kaspersky; a VPN kill switch). The
  probe's outside connection times out (-1) instead of being refused, and the session stops with "Required
  loopback/egress controls failed".

### Step 1: census

Status: the game's view is done and runs in every session; Windows' own view is still to do.

The plan: record every connection attempt the copy makes over whole sessions (start, ship, missions, co-op as host and
joiner, quit), with the block on, noting the module and call site, protocol, destination, port and outcome. Two views,
so nothing is missed:

- in the game: the mod loader logs Winsock's connect/WSAConnect/ConnectEx, sendto/WSASendTo, name lookups (getaddrinfo,
  GetAddrInfoExW), WinHTTP and WinINet, and the stand-ins' own sockets;
- Windows' own: the firewall's audit of blocked and allowed connections, and socket snapshots, for anything that does
  not pass through those calls.

Built 2026-10-19 (on branch `network-gate`, now in the engine), in the game's view. The `steam_api64.dll` stand-in,
which the game's executable imports and so loads before any of the game's own code, places hooks in its DllMain
(MinHook, `native/steam-cloud/net_gate.c`). They cover Winsock's connect, WSAConnect, WSAConnectByName, ConnectEx
(handed out through WSAIoctl), sendto, WSASendTo, WSASendMsg, getaddrinfo, GetAddrInfoW, GetAddrInfoExW and
gethostbyname; WinHTTP's WinHttpConnect; WinINet's InternetConnect and InternetOpenUrl; and kernelbase's CreateProcess
(the crash handler, `crs-handler.exe`, is a program of its own). WS2_32, WinHTTP and WinINet are imports of
`helldivers2.exe`, so they are hooked at start; a network module loaded later is hooked as it is mapped
(LdrRegisterDllNotification).

Every session writes `network-census.jsonl` (each attempt: call, target, loopback or outside, the calling module and
offset; repeats counted), and `result.json` holds its summary (`networkCensus`). `HellForge.Session census <log>`
summarises one from a player's logs. `native/steam-cloud/test/net_gate_test.c` tests it against the built stand-in.
The session turns the census on by default (`HELLFORGE_NETGATE=census`), with the send, dns, http and process hook
groups (see step 4 for why the connect groups are left out).

### Step 2: classify

Status: planned.

Sort every destination into loopback (the local backend), the Steam client (its IPC; co-op's Steam networking runs in
the client, not the game), and everything else, which must be none. Each destination is named and explained in this
doc.

### Step 3: catch

Status: planned.

What the census finds outside loopback and Steam is answered locally (routed to the backend) or refused, and a test
fails when a new one appears. Re-run the census for every new game build.

### Step 4: without Windows Firewall, in the game

Status: decided against as the blocker; the in-game gate stays an observer.

With the census complete, an in-game block (only loopback allowed from the copy's process, checked from inside the game
at every start, the session refused otherwise) could be an opt-in fallback where the firewall cannot be in force. It is
weaker than the firewall (code inside the game could avoid it), so the firewall stays the default. The user decides.

Finding, 2026-10-19 (in-game bisect, `HELLFORGE_NETGATE_HOOKS`): the in-process gate cannot block at the socket connect
level on this build. Hooking the connection primitives trips the game's own integrity check:

- the connectex group (the ConnectEx pointer from WSAIoctl) fast-fails the game about 24 s in (0xC0000409), even when
  its real function is hooked in place and the genuine pointer is returned, so the game checksums the code rather than
  validating the pointer;
- the connect group (ws2_32 connect/WSAConnect) does not crash but silently blocks sign-in;
- the send, dns, http and process groups are all fine (full sign-in, hundreds of backend requests).

getaddrinfo and sendto are ws2_32 too, yet are not checked, so only the connect/ConnectEx primitives are guarded.
Defeating that check is out of scope. The in-process gate is therefore an observer for dns, http, send and process
only. It cannot be the outbound TCP blocker, so it cannot replace the firewall on its own. The census default leaves the
connect groups out; they stay for a deliberate opt-in test and a re-test on future builds.

Decided 2026-10-19: the gate is the way off the firewall (no admin prompt, no clash with security suites, and, as
reasoned then, the Linux/Proton port would need neither firewall rules nor a network namespace). It must fail closed.
The gate lives in the stand-in, which the session already pins by hash and holds read-only, and the game imports it, so
it is hooked before the game runs. If any hook cannot be placed, the stand-in ends the process before the game's code
starts, and the session kills a game whose gate has not reported itself armed. The order: the census beside the
firewall, then the gate as an opt-in, and the gate as the default only once census data over several game builds shows
nothing but loopback and Steam. Since then, the in-game finding above made WFP (step 5) the way off the firewall, and
the Linux port blocks the copy with a network namespace ([linux.md](linux.md)).

### Step 5: off the firewall with WFP

Status: built; setup installs it, and sessions follow it. Still owed: a real wizard GUI run on a fresh install, and
co-op over WFP.

Started 2026-10-19 on branch `network-gate`. Since the in-process gate cannot block outbound TCP (step 4), HellForge
places the block that replaces the firewall straight into the Windows Filtering Platform (WFP), from outside the game's
process.

The shape: persistent WFP filters, installed once (elevated), checked each session and removed on uninstall. This is
the same install-once model as the firewall rules, so the session flow barely changes; there is no resident helper
holding a dynamic session. WFP is better than the firewall rules in two ways. It holds with Windows Firewall switched
off. And HellForge's filters sit in their own sublayer at the top weight (0xffff), so their BLOCK wins WFP's arbitration
over a security suite's or VPN's permit sublayer, which was the beta's Kaspersky/VPN problem.

The tool is `native/network-block/hellforge_wfp.c`, a Zig-built console tool (`hellforge_wfp install|uninstall|status`).
For each of the copy's programs it adds, over the four ALE layers (connect v4/v6, recv/accept v4/v6), a permit-loopback
filter above a block filter, keyed to the program's app id and tagged with a per-install provider GUID, so installations
never touch each other's blocks. On the connect layers the permit also names one remote port, the local backend's
(lockdown item 6): the copy reaches that loopback port and no other. Setup picks the port
(`InstallLayout.ComputedBackendPort`, 40000-46999 from the installation's path: below Windows' dynamic range, where
Hyper-V, WSL and Docker reserve blocks) and passes it to `Install-WfpBlock.ps1 -BackendPort`, which records it in the
marker (`network-block/wfp-block.json`, `"port"`); sessions start the backend on the marker's port. A marker without the
port is a block from before the narrowing, which setup places again, and so does a session (one Windows prompt; a
sandboxed session refuses to start without it). A round trip installs 8 filters per program, reads
them back and removes them cleanly. An enforcement test showed that a blocked program gets WSAEACCES (10013) to
192.0.2.1:443 and 10061 (not blocked) to loopback, the outcome the session's probe (OfflineControlNetworkProbe) checks.
Setup's probe also connects to the next loopback port and requires 10013 there, which proves the narrowing.
`scripts/Build-Engine.ps1` compiles, smoke-checks and packages it (its hash is `networkBlock` in `build.json`).

Proven over real boots (2026-10-19). `PlaySession.cs` takes an opt-in `HELLFORGE_WFP_ONLY`: the WFP filters (installed
beforehand) are the block, and the firewall rules are not required, installed or re-read. The egress probe is
unchanged and still gates the session, so `networkBlock` reads "wfp". Two boots of HellForge reached the ship:

- additive (firewall and WFP both on): 101 backend requests over loopback, external TCP 10013; WFP's loopback permit
  does not interfere;
- WFP only (firewall rules removed): `networkBlock` "wfp", external TCP 10013 from WFP alone, loopback passes, 102
  backend requests (war-season, stats and monetization endpoints), 135 s.

So WFP fully replaces the firewall for the boot-to-ship path. The test installation was restored to the firewall
afterwards.

Setup installs it (built 2026-10-19). The decision: the setup wizard places the WFP block and does not finish
until it is in place; a session then follows it, and defeating it from inside is out of scope. Setup's required "block
the copy from the internet" step runs `Install-WfpBlock.ps1` elevated. It places the filters (`hellforge_wfp`, with a
per-install provider GUID from the install path's hash), proves they enforce with the probe (outside refused, 10013),
and drops a marker (`network-block/wfp-block.json`); otherwise it fails loudly, and the wizard stops. A session that
finds the marker (or `HELLFORGE_WFP_ONLY=1`) takes the WFP mode, with no firewall rules required. The pre-copy check
needs only the Base Filtering Engine, not Windows Firewall, so a suite that switches the firewall off no longer blocks
setup. Installations without the marker keep the firewall path, so existing installations are unaffected. This was
verified elevated end to end (install, status, uninstall, the marker, enforcement).

Sessions deliberately do not re-check the filters. Reading or enumerating WFP filters needs administrator rights
(unlike Get-NetFirewallRule, which any user can read), so a non-elevated session cannot run the tool's `status`; it
trusts the install and the probe instead.

### Near term: the firewall path as it is

Status: planned; none of these is built yet.

- The prompt in front: the launcher asks, with its window as the prompt's owner (ProcessStartInfo.ErrorDialog and
  ErrorDialogParentHandle), brought to the front; never the windowless session runner. The launcher installs the block
  before starting a session when it is missing.
- Setup names a third-party firewall or security suite it finds (Windows Security Center: root/SecurityCenter2
  FirewallProduct and AntiVirusProduct) before the copy, and says what to do (Kaspersky, Norton, Bitdefender, ESET...).
- Messages in words: the probe's outcome (timed out: something lets traffic past Windows Firewall, such as a VPN or
  another firewall; no route; the local backend did not answer), the install script's own failure, a declined prompt,
  and an installer that wrote no report.
- `setup.log`: each step, and whether the prompt was asked, answered or declined.

## Co-op sync for mod content

Added 2026-10-18.

Status: steps 1 to 3 are built (engines 0.1.4 to 0.1.21); step 4 is planned; step 5 is partly done.

### Goal

A stratagem, entity, behaviour or ability a mod adds works in co-op with nothing done by the mod, as the game's own do.
[sdk-roadmap.md](sdk-roadmap.md) (principle 7 and phase 6) is the SDK's side of this.

### Found in the first co-op sessions (engine 0.1.2 and 0.1.3)

- Numbering by registration order, which is load order (the modlist code ignores order): registry.c's short ids and
  network entity types (fixed in 0.1.4: `WeaponOrder` by resource id); stratagem types (stratagems.c, STR_VANILLA +
  count, returned at add time; 160 vs 161 seen across PCs), ability ids (abilities.c, 2868+) and behaviour ids
  (behaviours.c, 694+), fixed in 0.1.4 by setting mods up in mod-id order (`SetupOrder`).
- HellForge code acting on one machine only: the silo countdown. In 0.1.3 (`ShadowFor`) every machine launched, which
  raced; in 0.1.4 only the machine with authority over the missile launched, as the game's silo does, and the others
  played the countdown.
- New behaviours: a Pelican on 694 (667-based) ran only on the caller's machine (its modloader.log: "behaviour 694
  runs"); a friend's ran only 695 (the turret), the host ran neither, and both saw it on the ground.

### Step 1: research

Status: done.

The research used the decoded game.dll image: how a remote machine starts and moves a stratagem's behaviour. It covered
the stage dispatcher 0x48EE50, the stage count 0x495F20 and the event <-> network slot pair 0x47A240 / 0x484B60 that
behaviours.c patches; who calls them for a replicated entity; and whether 667 runs on non-owners or its transform is
sent. Then came diagnostics in behaviours.c for the next session, where each machine logs stage changes and network
events for HellForge's ids, and later `coop-diagnostics.log` (engine 0.1.19; [multiplayer.md](multiplayer.md)).

### Step 2: fixed numbering everywhere

Status: done in 0.1.4 (`SetupOrder`, `QueueEdit` / `FinishSetupEdits`, `WeaponOrder`; unit-tested).

Code mods are initialised in a fixed order (mod id). The deferred edits (the fields.c queue) apply in load order, so
conflicts keep "the top mod wins". Event listeners and hooks run in a documented order. The fixtures in
`native/mod-loader/test/stats_test.c` check that added entities are numbered by resource id whatever the load order
(`WeaponOrderFixture`), and that code mods are set up in id order while their queued edits keep load order
(`SetupOrderFixture`). The test in the plan was two load orders giving identical stratagem types, ability and
behaviour ids, short ids and network types.

### Step 3: new behaviours and abilities sync as their base does

Status: built. The behaviour resume came in 0.1.4, the network apply switch for added types after the two-PC test of
2026-10-21, and the authority-only silo launch in 0.1.21. A two-PC confirmation is not recorded in this repository.

Found (2026-10-18): without authority, the record is made empty at the spawn (0x842200 skips its set-up) and filled from
what arrives (0x842690: id, stage, fields), the transport is not updated there, and each arrival ends with resume
(0x493BE0), which for 667 sends the mover to the replicated flight target P+0x1BC. Built in 0.1.4: a seventh patched site
(resume's jg at 0x493C00; the ja at 0x4952BC straddles a qword) sends a new id into the switch with its base's index,
and the transport's approach marks P+0x1BC replicated (0x8449B0, 0xE00) as the orbit step does. It was unit-tested (the
stub run on a synthetic game).

The two-PC test of 2026-10-21 (0.1.8) still found added content out of step: a friend's Pelican never ran its
behaviours on the others' machines, a silo missile launched on one machine stayed or vanished on the other, and a
host's barrage and custom support weapon were not seen by the friend. Engine 0.1.19 made the silo launch on every machine
again, which set the explosive off once per machine. Two fixes followed:

- the network apply switch (`native/mod-loader/netapply.c`, "co-op fix 1"): the game applies another machine's state
  through a switch sized to the vanilla network types, so every added type took the exit and got none of its state.
  HellForge moves the table and gives each added type its base type's case;
- engine 0.1.21 ("co-op fix 2", `native/mod-loader/abilities.c`): as the game's own silo, only the machine with
  authority over the missile launches it, and the launch reaches the others with the missile's state.

### Step 4: authority and a mod message channel

Status: planned; neither `is_authority` nor `send` exists yet. (The spawn event, `HfSpawnEvent`, already says whether
this machine simulates the entity: its `authority` field.)

`is_authority(entity)` and a mod message channel (`send`, and an event for what arrives). Messages go from the loader to
the backend over the session pipe (a game -> backend direction to be added), from the backend to friends' backends as a
frame (a protocol bump), and back to the same mod on their machines. The countdown's laser point and the turret's
target move onto it.

### Step 5: the modding docs' co-op section

Status: partly done. [modding/concepts.md](modding/concepts.md) has a co-op section, and
[modding/hooks.md](modding/hooks.md) and [modding/content.md](modding/content.md) have co-op notes; the message channel
waits for step 4.
