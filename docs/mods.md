# Using mods

<!-- diataxis: how-to -->

For players: how to add, order and remove mods in HellForge, and what friends need to play together. To make mods, see
[modding/README.md](modding/README.md). Mod managers and launchers of their own use
[external-launchers.md](external-launchers.md).

## What HellForge takes

Mods live in `<installation>/mods`, each as a folder or a `.zip` of one. HellForge takes:

- HellForge mods: a `hellforge-mod.json` with code (sources HellForge builds for you), data, stats, or several of them.
- Cosmetic packs in the community's format (a `manifest.json` with options), as other mod managers install them. The
  folders and images a manifest names are matched as Windows matches them, on Linux too: whatever their case, and with
  trailing dots or spaces left off (a pack's `PGO-7V3.` is the folder `PGO-7V3`).
- Lua mods: HD2Runtime, Bingus Shared Loader and the mods made for them ([lua-bridge.md](lua-bridge.md)).

A code mod runs inside the game with your permissions, so only enable code mods from people you trust. The Mods page
marks them CODE.

## Adding mods

1. Open the launcher's Mods page and press Add mods....
2. Pick one or more files: a mod's `.zip` (or `.rar`, `.7z`), a folder mod's `hellforge-mod.json`, or a cosmetic pack's
   `manifest.json`. HellForge copies each mod into the mods folder.
3. A mod may have the same id as one already installed: an update, or going back to an older version. The page then
   lists it and waits. Press Add to replace the installed one, whose files are deleted, or Cancel.

Copying a mod into the mods folder yourself works too (Open mods folder opens it), and the page shows it at once. A
`.zip` loads as it is. A `.rar` or `.7z`, and a zipped cosmetic pack, is unpacked into a folder of its own and the
archive is removed, the next time the Mods page loads or a session starts.

When a dropped archive holds a mod that is installed already, it stays as it is and Mod notices says so: use Add mods...
to replace the installed one. When every file it holds is in the installed mod with the same bytes, it is only a second
copy: it is removed, and Mod notices says so.

Some things to know:

- `.rar` (RAR 4 and 5) and `.7z` files are unpacked by HellForge itself, on Windows 10 and 11 and under Wine. Only plain
  files and folders come out. A password-protected archive, one split into parts, or one with links or paths outside
  itself is refused: unpack it yourself and add its folder.
- Keep a gameplay mod as its zip: HellForge unpacks it by itself. Two copies of one mod (a zip and a folder with the same
  id) load neither, and the page says so.
- Cosmetic packs are kept as folders: a zipped pack becomes one, with its options and its place in the order. A zip kept
  beside its unpacked copy would hold the pack twice.
- Files that are not mods are listed under Mod notices with the reason, until the next Add mods.

### Disk space

PLAY puts each mod's patches into the game copy's data folder as hard links to the mod's own files where it can, not as
second copies:

- **Windows:** every patch. While the game runs, a mod's patch files cannot be saved over, as the game's cannot.
- **Linux:** the files HellForge keeps itself (zips' unpacked copies, mods from hosts). A mod folder's patches are copied,
  since Linux lets a program write to a file the game is reading.

A drive without hard links (FAT32, exFAT) gets copies. A gameplay mod kept as a zip is on the drive twice, as its zip and
its unpacked copy: the zip is what friends compare. Explorer's folder sizes count a linked file once in each folder, so
the drive's free space is what shows the saving.

A staged patch is the mod's file under a second name, so change the mod, not the file in the game's data folder. Written
over there, the mod's file changes too. The next session start finds it: a file HellForge keeps itself is made again,
and for a mod folder Mod notices says which file changed, so you can reinstall the mod.

## The Mods page

Every mod is a row with a switch, in two groups: GAMEPLAY mods first, then CLIENT ONLY ones. Client-only mods are
cosmetic packs, Lua mods and mods that say they are not gameplay mods; friends need not have the same ones.

- Click a row's name for what the mod changes. Its ⋮ menu (or a right click) has Details, Move up and Move down, and
  Open folder. It also has Build now for a mod with sources, Prepare for sharing for your own folder mods, and Remove... for
  your own mods (not the bundled mods or Local Stats). Remove asks before it deletes the mod's files.
- Tags on the right say what a mod is, among them CODE, DATA, STATS, PACK, LUA, ZIP and BUNDLED (shipped with
  HellForge). DID NOT COMPILE marks a source mod whose last build failed; it stays off until fixed (or Build now compiles
  it again), and its details show the compiler's message. A build that failed in the mod tools (Zig), not in the mod, is
  not marked: the next session unpacks Zig again and builds the mod.
- PREMIUM marks a mod built on Arrowhead's premium content (a premium warbond, Superstore items, a Steam DLC). What it
  adds is yours only when you bought that content in the normal game and synced it (Profile > Account sync). PREMIUM:
  LOCKED means the selected profile's synced account lacks it: the mod loads, but its weapons and stratagems stay
  locked, and a cosmetic one is not loaded. Its details say what is missing.
- A pack's details let you switch its options on and off (or All on, All off) and pick their variants.
- Mod notices, at the top in red, say why a mod could not be loaded.
- The modlist code friends must match is at the bottom of the page.

Changes apply to the next session. While a session runs, a switch the running game can take changes it at once, and
everything else waits for the next session ([Changing mods while you play](#changing-mods-while-you-play)).

While a session runs, its mod files are held read-only, so nothing the game runs can change a mod that loads as yours
next time. HellForge's own files stay writable:

- the page's settings file
- Local Stats
- the in-game editor's settings, presets and the host's files beside it
- the mod sync downloads in `mods/.synced`, each checked again, by its hash, whenever a session would use it.

When the session ends, anything else that appeared in `mods/` is listed in the Console. A new mod, or a mod whose
folder gained files, is switched off until you switch it on here. Only switch it on again if you put those files there.

The page saves its switches, order and pack options in `mods/hellforge-mods.json`.

## Changing mods while you play

This is experimental and off by default: switch on Settings > Experimental > Change mods without restarting, which
applies from the next session. Off, a change waits for the next session, and joining a friend with other mods
restarts the game.

While such a session runs, the Mods page's switches still work. Switching a mod on or off, or saving a change to its
files, goes to the running game, and the console says what changed:

- **Stats mods** (`hellforge-stats.json`, `componentWrites`, `"own": true` records). Each value applies as a live edit
  does: now, on respawn, or at the next mission; the stats editor shows which. A changed mod keeps its place in the load
  order. The mods above it still win where they change the same value, Local Stats included. A code mod's edit of a
  field stays on top of the stats under it.
- **Network field types** widen, or narrow back, at once.
- **Code mods that declare `"live": true`** are set up while the game runs, and taken out again. A source mod is built
  first, which can take a moment. New stratagems, weapons, entities, projectiles, damage types, behaviours and abilities
  they add are numbered as a fresh start with your mods would number them, and the game owns their items at once.
- **What a code mod taken out added** stays out of sight until the mod comes back: out of the armory's items you own,
  the stratagem picker and your loadout. Its stratagems', projectiles', damage types', behaviours' and abilities'
  numbers pass to another mod's new ones where a fresh start would number those. So joining a host whose content mods
  differ from yours changes them live. Its weapons and entities keep their numbers until the next session.
- **Data mods** (game patches and data specs) reach the game as a package of their own. Objects already in the world
  keep their old look until they are made again (a respawn, the next mission).
- **A level 11 difficulty** is added at once when it has no larger maps or extra objectives; it can be picked once no
  menu is open. Hell Forge's larger maps need a new session, as does taking a difficulty out.
- **Everything else is saved for the next session,** and the page says why. That is:
  - code without `"live": true`
  - Lua mods
  - cosmetic packs
  - a new order
  - whatever the game refuses. For example, content a fresh start would number another way, because a mod that stays
    adds something before it. Or a new weapon or entity where a mod taken out this session had its number.
- **A stratagem that takes over a removed one's number** waits for the loadout screen to close.
- **The order and packs' options** stay locked while a session runs.
- **Only while no friend is in your game, on the ship or the title screen.** Friends need the same mods, so the game
  refuses a change while one is connected; stats alone also change in a mission. After a change, friends who join need
  your new mods, which mod sync offers them.
- **A session that plays a host's mods** (after a mod sync) keeps them. Your Mods page applies to your next session.

How it works:

- **The session's files.** The session's backend compares the mods the session runs (`session-mods.json` in its log
  folder) with the Mods page's (HellForge.Core `LiveModPlan`).
- **Builds and data.** A change with source mods or game data is prepared first by `HellForge.Cli mods-live`: it builds
  the mods and writes the change's package into the game copy's `data` folder (the next session removes it).
- **One change.** It sends the in-game loader the whole change over the live pipe, and the loader takes all of it or
  none. A change the loader refuses is saved for the next session, as before.
- **Afterwards.** The session's modlist code, `modset.json`, its lobby and its public listing follow the new mods.
- **Logs.** Each change is logged in the session's `backend/live-mods.log`, and the loader's side in `modloader.log`.

## Load order

Drag a row within its group to change the order, or use Move up and Move down. The bottom mod loads first and the top
one wins, as in other mod managers: where two mods change the same thing, the higher one's change stays. Mods new to the
page go to the top of their group. The in-game editor's saved changes (Local Stats) are not on the page: they are always
on, load on top of every mod so they apply last, and friends never need to match them. Reset a change in the editor to
undo it.

## Playing with friends

Friends who play together need the same **shared** gameplay mods: the same mods, the same versions and the same files.
The Mods page shows the modlist code to compare. With no shared gameplay mods there is nothing to compare, and you can
play with friends who have none.

The order is up to each of you. These are your own too, and friends need not match them:

- cosmetic mods, packs and Lua mods
- **per-player** gameplay mods: stats-only mods, texture/sound/text-only patch mods, and mods whose manifest says
  `"coop": "per-player"`. The Mods page puts those in the PER PLAYER group ("Only on your PC: friends do not need them,
  and each player plays with their own"), and they do not change the modlist code.

A manifest can also force `"coop": "shared"`. One that declares per-player but changes the wire (network field types)
or numbered resources (a data spec) is refused.

A mod whose manifest has a `"coopLevel"` counts by that level rather than its files (bundled HellForge mods all have
one). So friends on different releases of it play together, while a level change splits them.

Players of HellForge 0.3 can join you and you them: their lobbies appear in the Galactic Map and the lobby browser, and
HellForge speaks their version's protocol. If your mods differ, the MOD SYNC prompt works as with anyone. A lobby with
one of HellForge's bundled gameplay mods on (Hell Forge's difficulty, Vanilla Plus) is not listed for 0.3 players.
Friends who run the same mod still join it.

When a friend's code is not the host's, the join is refused and both games say why, over the game:

- The joiner sees "Couldn't join \<host>" for 20 seconds, with each mod that differs: one the host has and they do not,
  one only they have, another version, or the same version with other files.
- The host sees a small note in the top right corner for 6 seconds: "\<friend> couldn't join" and the reason in one line
  (the full list is in the host's `backend/multiplayer.log`).

A different HellForge version is reported the same way, and the joiner is told when a lobby is full or locked. The
bundled in-game editor draws these notices. With it switched off, only the logs say why: `backend/multiplayer.log` in
each session's folder, the first lines of the list.

A join is accepted even when per-player mods differ. After it, if the host plays with per-player mods this player lacks,
the joiner gets one brief notice ("\<host> plays with 2 mods you don't: \<names>. Each player keeps their own."); nothing
downloads for those. The host's set lists them with `"tier": "per-player"`, and older sets without a tier read as
shared.

When the host lets joiners download their mods, the joiner also gets the MOD SYNC prompt (below).

In co-op every player keeps their own Local Stats (the in-game editor's saved changes) unless the host shares theirs
([Co-op in game](multiplayer-in-game.md#stats-in-co-op)), so yours is not part of the modlist code.

## Playing with other mods

Joining a lobby whose host runs other gameplay mods does not just fail. The host's backend refuses the join (above) and
sends its mod set with the refusal. A **MOD SYNC** prompt then shows over the game by itself, without the editor
window behind it (the in-game editor draws it). It holds the mouse and keyboard until it is answered, and lists each of
the host's mods and what happens to it.

With the in-game editor switched off on the Mods page, the launcher's Play page shows the same prompt instead, with the
same buttons (BLOCK is the in-game prompt's only):

| Shown as | Meaning |
|---|---|
| you have it | an installed mod (on or off) or one downloaded before has the same id and content |
| downloads | it comes from the host's backend over the Steam peer link |
| needs your trust | it runs code or carries data that HellForge cannot check, HellForge has not verified it, and the host is your Steam friend or your game is sandboxed: **TRUST** downloads it (below), or you install it yourself |
| install it yourself | the host does not share their mods, or it runs code or carries data that HellForge cannot check and has not verified, the host is not your Steam friend, and your game is not sandboxed |
| blocked | it is on your blocklist, so the lobby cannot be joined |
| needs another HellForge | a mod HellForge ships differs: both players need the same HellForge |
| off for this session | one of your own gameplay mods that the host does not run |

Its buttons, **Enter** and **B** wait a second after it appears, so a key or click meant for the game does not
answer it. **DOWNLOAD AND RESTART** downloads what it can, when the drive has room for it and 256 MB to spare.
**Esc** hides the prompt while the download goes on, and it comes back when the mods are ready. **CANCEL** or **Esc**
before that answers no.

Large mods are mostly the game's own files (models, textures and sounds copied from the game), and your game has
those. From a host on this HellForge, those parts are written from your own game files and only the rest downloads.
How much that saves depends on how much of the mod is the game's own.

**Index my game** on the Mods page (MOD SYNC, under "When I join") lets it find the game's files a mod renamed too. It
takes about a minute per game build; run it while no session is running. Every file is still checked against the host's, byte for byte ([how it works](multiplayer-mod-sync.md#mod-sync-from-your-own-game)).

Then one of two buttons plays the host's mods, in the host's load order, plus your own cosmetic mods and Local Stats:

- **APPLY AND JOIN** when the running game can take every difference with the host's mods (see
  [Changing mods while you play](#changing-mods-while-you-play)). The game changes its mods without a restart, and the
  host's invitation to their lobby comes at once.
- **RESTART TO JOIN** otherwise: it ends the session and the launcher starts a new one with the host's mods. Once the
  game is on the ship, the host's invitation waits in game. A change the running game refuses restarts the same way.

Accept the invitation ("Rejoining \<host>") to join. If the lobby has closed, is full or locked, or the host does not
answer, the game says so ("Couldn't rejoin \<host>"); join from the social tab or the map then. Only that one session
plays the host's mods: your Mods page and its switches never change, and the next session you start yourself runs your
own mods again.

### What may download (the trust rule)

Only what HellForge checks itself downloads by itself. Your backend decides each mod's class from the bytes it
received, never from what the host says. The rule is the same for every host, Steam friends included. Only who can be
trusted differs: a Steam friend's code and raw data, never a stranger's.

- *Content* downloads from any host. It is:
  - data specs HellForge checks all of (`hellforge-data.json`), whose patches HellForge builds from your own game
    files. They hold icons, and copies and includes of units, materials, textures and particle effects with only
    HellForge's own edits, each on its own type, and includes of the game's sound banks.
  - schema stats (`hellforge-stats.json` fields by name, each within its reviewed range)
  - network field types (an integer field widened, never past 31 bits).
- *Raw data* reaches the game without HellForge being able to check it. It downloads only as code does, below. It is:
  - binary data patches (a `data/` folder, read by the game's own file parsers)
  - data specs that `"replace"` values (8 bytes of the author's choosing written into a copied resource), or that copy
    or include any other resource type (`lua` among them)
  - raw stats lines (the stats file's `"raw"` section)
  - `componentWrites` (any field of a component record).
- *Code* is a DLL, C or C++ sources HellForge compiles, or a patch holding Lua. Code and raw data download only when
  HellForge's verified list names that exact content, from any host, or when you trusted it (below).
- *Bundled* mods (the in-game editor) never download.

A Steam friend's unverified code or raw data can be trusted. So can anyone's while your game is sandboxed. Otherwise a
stranger's is one you install yourself. The prompt marks these mods CODE or RAW DATA.

The verified list is `data/verified-mods.json`, shipped with each HellForge release, with entries
`{"id","name","version","hash","reviewed"}`. It names the Stratagem Compendium 1.0.4. The hash is the mod's content
hash: the SHA-256 of the lines the modlist code takes from it (`ModSets.ContentHash`; `HellForge.Cli mod-hash` prints
it).

### Trusting a mod

A row that needs your trust has a **TRUST** button (or **T** on the selected row). It opens a warning in the prompt's
place:

> *\<Mod> \<version>* runs its own code on your PC. A mod like this can do anything your Windows account can: read and
> change your files, or install other programs. Only trust it if you trust *\<friend>* and know where the mod comes from.

The warning differs for two kinds of mod:

- A raw data mod's warning says it carries game data HellForge cannot check, which can break the game or, through a
  flaw in it, run code on your PC.
- A stranger's says the sandbox stops the mod changing your files or reaching the internet, but not reading your files
  and sending what it reads to them through the game.

Its **TRUST AND DOWNLOAD** button waits a second and answers only to a click: Enter does nothing there, and **CANCEL**
or **Esc** goes back to the list. Trusting the last mod that waits starts the download at once. With others still
waiting the button says **TRUST**, and the prompt comes back with that mod set to download.

What a trust covers:

- **One exact content.** Trust is for that exact content: its id and content hash, from that lobby or any other.
  Another version, or the same version with other files, has to be trusted again.
- **Only while trusted or verified.** A downloaded code or raw data mod counts as yours, and loads, only while
  HellForge's verified list names it or you trust it. One taken off the list, or untrusted, has to be trusted again
  before it plays.
- **A stranger's mod stays in the sandbox.** A mod you trusted from a stranger runs only while your game is sandboxed.
- **So does a trust given in the sandbox.** A mod you trusted in a sandboxed game, a Steam friend's too, also runs only
  while your game is sandboxed. Any mod running in the game can answer the prompt as you would, so a trust given there
  must not let a mod out of the sandbox later. The warning says so, and the Mods page marks it "sandbox only".
- **Trusting again.** Trust it again outside the sandbox, unsandboxed or on the launcher's own prompt, to play it there.
  Trusting a mod again never narrows a trust you gave it before.
- **Where it is kept.** Trust lives with your mods (`mods/hellforge-mods.json`), so a HellForge update leaves it alone.
  A mod you trusted downloads from any host after that with no prompt. Everything is still checked as below.
- **Taking it back.** Trusted mods are listed on the launcher's Mods page (MOD SYNC card) with who hosted them and
  when. **Untrust** takes the trust back and deletes the downloaded copy.

### Getting a mod yourself

A mod that waits for your trust or is yours to install has a **GET IT** button (or **G**) when its author gave it an
address (`url` in its manifest, an `https://` address). It opens the page in your browser; the game stays as it is. To
install the mod, close the game, add the mod on the launcher's Mods page, then start HellForge again and join. The Mods
page also links a mod's address in its details.

### Checks

Every download is checked in this order:

1. The file list, the manifest (`hellforge-mod.json` before `bts-mod.json`, as HellForge reads them), the stats file
   and the data spec come first. A mod that shows code or raw data there and is neither verified nor trusted stops
   before its other files.
2. Every file must have a safe path and match the size and SHA-256 the host listed.
3. The finished mod must parse, have the expected id and version, rehash to the host's content hash, and be content,
   verified or trusted.
4. Only then is it kept, in `mods/.synced/<id>@<hash>/`.

A failed or cancelled download is deleted, and anything the host sends that does not fit ends the download with a
message. A kept mod is used again only while its files still hash to the same content. Normal sessions never load that
folder.

### Blocking

**BLOCK** on a row of the prompt (or **B**) puts that mod on your blocklist: lobbies running it are not offered again.
Unblock it on the launcher's Mods page (MOD SYNC card).

### Hosting

Sharing is on by default. Turn it off with "Share my gameplay mods with players who join" on the Mods page.

Players who join you may download your gameplay mods' counted files for 10 minutes after their first refused join
(joining again does not extend it), and at most your mods' size twice over. Anyone who joins can take them, Steam friend
or not: it is up to each of them to trust mods that run code. A source mod's sources go with it, so turn sharing off
before hosting with mods or sources you have not released. Your backend serves at most 8 MB/s per player and 16 MB/s in
total, so your own game keeps its link.

### Downloaded mods

HellForge keeps the 20 used most recently (played, or downloaded) and removes older ones when a session starts, never
one that session plays. The Mods page shows how many there are and how much space they take, with a button that
deletes them all.

### What mod sync does not cover

- Settings a mod keeps outside its own folder are neither compared nor downloaded, so they can differ between host and
  joiner.
- A mod that writes into its own folder while it runs changes its own files, and with them its content hash. A synced
  copy that does so no longer matches the host's, and the next sync falls back to your own mods.

### Public lobbies on the Galactic Map

The map lists other HellForge players' public lobbies, except the ones you could not join:

- lobbies running a blocked mod;
- lobbies running mods that would have to be installed by hand (mods that wait for your trust still show: a Steam
  friend's, or with the sandbox on, a stranger's);
- lobbies whose host enforces their stat changes (Local Stats, the in-game editor's values or arm swaps). Joiners play
  with the host's stats, so these are hidden unless "Show lobbies with host-enforced stat changes" is on.

Lobbies with other mods you can download still show. Joining one shows the MOD SYNC prompt.

Only lobbies whose host set Privacy to Public are listed. To keep your own Public lobbies off other players' maps, turn
off "List my Public lobbies on the Galactic Map" on the Mods page. Who may join each privacy setting is in
[multiplayer.md](multiplayer.md#how-co-op-works-for-players).

### The lobby browser

The ship's information terminal, across from the Armory, opens a lobby browser instead of the news and war screen,
which needs Arrowhead's servers. The camera glides up to the terminal's screen and the browser fills it. It lists every
HellForge lobby, on any planet and difficulty, including the ones the map leaves out. It says what joining each would
take: ready, a download, your trust, or why you can't.

- Filter it to joinable lobbies (J), friends' (F) or a minimum difficulty (D).
- Enter or JOIN sends you the lobby's invitation, which you accept in game.
- QUICK PLAY (Q) picks for you: a friend's lobby first, then one you can join with the fewest downloads.
- Strangers' lobbies show even while Play with strangers is off. Select one and press A to turn it on (after a warning
  about what that risks).
- In the in-game editor (F8), Ctrl+L opens it too.

Without the editor the terminal shows the game's own plain list of lobbies.

### For mod authors

The prompt is the SDK 14 calls `sync_state`, `sync_mod` and `sync_answer`, and the lobby browser the SDK 16 calls
`browse_state`, `browse_lobby` and `browse_ask` ([sdk-reference.md](sdk-reference.md)).

## See also

If a mod does not show up in game, see [troubleshooting](troubleshooting.md#sessions-that-fail). For the docs map, see
[README.md](README.md).
