# The published verified list

For maintainers: how HellForge takes its verified list from files.hf-mod.com, so a mod is added or taken off without a
HellForge release, and how to publish one.

The verified list names the code and raw-data mods a maintainer has reviewed, each by its exact content hash
([adding a mod](modding/tools.md#the-verified-list)). Mod sync downloads a mod on it from any host without asking the
player, so the list decides what code runs on players' PCs. That is why a published list must be signed, and why the
signing key never goes to the server.

## How HellForge finds the list

Before each session the session runner fetches `https://files.hf-mod.com/verified-mods/verified-mods.json` and its
signature, `verified-mods.json.sig`, within 5 seconds (`VerifiedMods.RefreshAsync`). It takes the list only when:

- the signature is the maintainers' key's signature of the file's exact bytes (ECDSA P-256 with SHA-256, the IEEE P1363
  form, base64), checked against the public key in `VerifiedMods.PublicKey`;
- the list parses, every entry is well formed, and it has a `"serial"` of 1 or more;
- its serial is not below the one this installation saved last.

It then saves both in `<installation>/temp/verified-mods.json`. The session's `HellForge.Cli mods-stage` and backend read
that copy and check its signature again (`VerifiedMods.UseInstallation`). The list in use is the saved one when its
serial is above the list built into the release (`data/verified-mods.json`), else the built-in one. When the list can't
be fetched or is refused, the saved copy stays, and failing that the built-in list. The session's `result.json` says which
(`verifiedMods`: `source`, `serial`, `problem`), and the backend's `multiplayer.log` says which list mod sync uses.

The serial stops an old signed list from coming back. Once an installation has seen serial 5, a copy of serial 4 (which
may still list a mod taken off since) is refused. An installation that has never seen serial 5 could still be given
serial 4, so the release's built-in list gets the latest serial with each release.

While `VerifiedMods.PublicKey` is empty, nothing is fetched and only the built-in list counts.

## The signing key

Make the key pair once, on the PC that will sign the lists:

```powershell
HellForge.Cli verified-list-keygen --out <a folder outside the repository>\verified-list-key.pem
```

It prints the public key. Put it in `VerifiedMods.PublicKey` (`src/HellForge.Core/VerifiedMods.cs`). Releases from then
on take lists signed with that key.

- The private key never goes to files.hf-mod.com, this repository, CI or a chat. With it, anyone can make every
  HellForge player run their code, so whoever can sign has to be someone who reviews mods.
- Keep a backup somewhere offline. A lost key means a release with a new public key before the list can change again.
- If the key leaks, release with a new public key and a built-in list whose serial is above every list signed so far.

## Publishing a list

1. Edit the list: the published one (or `data/verified-mods.json`), with the mod's entry added or taken off, and
   `"serial"` one above the last published serial.
2. Sign it: `HellForge.Cli verified-list-sign --list verified-mods.json --key <the private key>`. It writes
   `verified-mods.json.sig` and prints the serial, the number of mods, and whether the key is the one this HellForge
   carries (`hellForgeKey`).
3. Check it as a player's HellForge will: `HellForge.Cli verified-list-check --list verified-mods.json`.
4. Upload `verified-mods.json` and `verified-mods.json.sig` to `verified-mods/` on files.hf-mod.com. Between the two
   uploads, sessions keep the list they saved.
5. Put the same list in `data/verified-mods.json` in the repository, so the next release builds it in.

Upload the file exactly as signed: a changed line ending is a changed list, and its signature no longer matches.
