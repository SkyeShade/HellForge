# Co-op and public lobbies

<!-- diataxis: how-to -->

This page is for players: how to play HellForge co-op with friends and in public lobbies, and which settings decide who
can join. How co-op works inside HellForge is on the contributors' pages listed at the end.

Players, each running HellForge with their own local backend, play together through the game's own social menu, join
flow and Galactic Map. Friends find each other through Steam presence. Public lobbies are listed on Steam's lobby
service, never on Arrowhead's. There is no port forwarding and no central HellForge server. The copied game never
reaches PlayFab, Arrowhead's services or vanilla players' sessions.

## How co-op works for players

1. In the launcher, open Settings. The MULTIPLAYER card has two switches, "Local PlayFab" and "Lobby and session through
   Steam friends".
   - Both are on by default, and both are needed.
   - A change applies to the next session.
   - The launcher's console still calls them experimental.
2. Compare modlist codes. The Mods page shows the code.
   - Friends need the same gameplay mods, in the same versions with the same files. The order is up to each player.
   - Cosmetic mods, packs and Lua mods are each player's own.
   - [mods.md](mods.md#playing-with-friends) describes what each player sees when a join is refused.
3. When the host shares their mods, the joiner can download them and play with them instead
   ([mods.md](mods.md#playing-with-other-mods)).
   - When it can, the running game takes them without a restart (APPLY AND JOIN), and the host's invitation comes at
     once.
   - Otherwise the game restarts, and once it is on the ship the invitation waits in game. Accept it to join.
4. Start a session. In the game's social menu, the friends list shows Steam friends who are playing HELLDIVERS 2 with
   HellForge running, and you join their game from there. Steam sign-in, Steam friends and Steam lobbies are the real
   ones.
5. In a friend's lobby, every player keeps their own in-game editor changes and can keep making them.
   - HellForge loads what each player's changes need on every machine (a vehicle's swapped weapon, a pod's pickup), so
     friends don't see missing assets.
   - The joiner's editor header shows "JOINED · \<name> · YOUR OWN STATS", and the host's shows "HOSTING · EVERYONE
     KEEPS THEIR OWN".
6. A host can instead have everyone play with the host's changes: "Everyone plays with my editor changes" under "When I
   host" on the launcher's Mods page (MOD SYNC card).
   - A joiner's editor then turns grey and read-only, with "READ-ONLY · HOST: \<name>".
   - The host's shows "HOSTING · FRIENDS PLAY WITH YOUR STATS".
   - [Stats in co-op](multiplayer-in-game.md#stats-in-co-op) says what each way means for what other players see.
7. When the host leaves, the lobby closes for everyone, as HD2's host ends the session. If you were playing with the
   host's stats, your own come back.
8. Who may reach you at all is "Play with strangers" (Settings > MULTIPLAYER).
   - It is on by default while your next session is sandboxed, and off when it is not, unless you have chosen on or off
     yourself.
   - Off, only your Steam friends reach your backend, and the Galactic Map shows your friends' public lobbies only.
   - [Play with strangers](multiplayer-lobbies.md#play-with-strangers) says what turning it on risks.
9. The game's own Privacy setting (Options) decides who may join a lobby you host:
   - **Public:** with Play with strangers on, any HellForge player. With it off, your Steam friends only, and the lobby
     is not listed.
   - **Friends only:** your Steam friends, and players you invite. It is not listed.
   - **Invite only:** players you invite from the game's social menu. Friends you have not invited are refused too. It
     is not listed.

   Changing the setting during a session never removes anyone: players already in the lobby stay, and a player who was
   in it (whose game crashed, say) may come back. A refused player's game says why ("The host's lobby is Friends only").
10. A listed Public lobby shows on other HellForge players' Galactic Map, and they can join it from there.
    - It shows its players' names and the host's Steam account id.
    - Lobbies a player could not join are left off their map: a blocked mod, mods to install by hand, or a host who
      shares edited stats unless the player chose to show those.
    - A Steam friend's lobby is shown even when it runs code mods HellForge has not checked (when the friend shares
      their mods, the default). Joining it brings the MOD SYNC prompt, where you can trust them.
11. To keep your Public lobbies off other players' Galactic Map, turn off "List my Public lobbies on the Galactic Map"
    on the launcher's Mods page (MOD SYNC card). It is on by default. Your Public lobby then still takes anyone who
    joins it, but players who are not your friends cannot find it.
12. Invitations from the game's social menu reach Steam friends running HellForge, and the invited player may join even
    an Invite only lobby.
13. When the host kicks a player:
    - that player's game leaves the lobby and says "Removed from \<host>'s lobby";
    - their game traffic is cut off at once;
    - they cannot join that lobby again while it lasts.

For a bug report, collect both players' logs (launcher Console > "Save logs to Desktop"). Each session's logs hold:

- `backend/multiplayer.log`: the backend's side of co-op;
- `coop-diagnostics.log`: what this machine's game saw of mod content.

## For contributors

How co-op works inside HellForge is on four pages:

- [Co-op internals](multiplayer-internals.md): what a session starts, the stand-in DLLs, the backends and their Steam
  link, joins, privacy, invitations and kicks, and the research into how vanilla HD2 does co-op.
- [Co-op mod sync](multiplayer-mod-sync.md): how a joiner takes the host's mods, its speed, the rejoin after a restart,
  and what another player's mods can reach.
- [Public lobbies and strangers](multiplayer-lobbies.md): the lobby directory, the Galactic Map's search, the lobby
  browser, HellForge 0.3 players and Play with strangers.
- [Co-op in game](multiplayer-in-game.md): stats, assets, pickups and shots in co-op, the mod channel and mod content.

The research and build log of October 2026 is in
[the page's earlier version](https://github.com/SkyeShade/HellForge/blob/8960d58f03787264e3ce88aa68cffaaf8af0d8ff/docs/multiplayer.md#history-research-and-build-log-2026-10-14-to-2026-10-23).
Its findings that still hold are on the pages above.

## Moved sections

Code comments and other pages link to these headings. Each section now lives on a contributors' page.

### Mod sync

Moved to [Co-op mod sync](multiplayer-mod-sync.md#mod-sync).

### Mod sync speed

Moved to [Co-op mod sync](multiplayer-mod-sync.md#mod-sync-speed).

### Play with strangers

Moved to [Public lobbies and strangers](multiplayer-lobbies.md#play-with-strangers).

### The lobby browser

Moved to [Public lobbies and strangers](multiplayer-lobbies.md#the-lobby-browser).

### The ship's information terminal

Moved to [Public lobbies and strangers](multiplayer-lobbies.md#the-ships-information-terminal).

### Stats in co-op

Moved to [Co-op in game](multiplayer-in-game.md#stats-in-co-op).

### Assets in co-op

Moved to [Co-op in game](multiplayer-in-game.md#assets-in-co-op).

### Shots in co-op

Moved to [Co-op in game](multiplayer-in-game.md#shots-in-co-op).

### The mod channel in co-op

Moved to [Co-op in game](multiplayer-in-game.md#the-mod-channel-in-co-op).

### Step 5: stats in co-op, the host's values everywhere (2026-10-17)

What step 5 built is in [Stats in co-op](multiplayer-in-game.md#stats-in-co-op). Its two-PC test plan is in
[the build log](https://github.com/SkyeShade/HellForge/blob/8960d58f03787264e3ce88aa68cffaaf8af0d8ff/docs/multiplayer.md#step-5-stats-in-co-op-the-hosts-values-everywhere-2026-10-17).

### Step 7: public lobbies on the Galactic Map (2026-10-22)

What step 7 built is in [Public lobbies and strangers](multiplayer-lobbies.md#public-lobbies-the-lobby-directory),
with its findings under [Testing public lobbies](multiplayer-lobbies.md#testing-public-lobbies). Its two-PC test plan is
in [the build log](https://github.com/SkyeShade/HellForge/blob/8960d58f03787264e3ce88aa68cffaaf8af0d8ff/docs/multiplayer.md#step-7-public-lobbies-on-the-galactic-map-2026-10-22).

### Step 10: every player's own stats, and the lobby's assets (2026-10-23)

What step 10 built is in [Stats in co-op](multiplayer-in-game.md#stats-in-co-op) and
[Assets in co-op](multiplayer-in-game.md#assets-in-co-op). Its two-PC test plan is in
[the build log](https://github.com/SkyeShade/HellForge/blob/8960d58f03787264e3ce88aa68cffaaf8af0d8ff/docs/multiplayer.md#step-10-every-players-own-stats-and-the-lobbys-assets-2026-10-23).
