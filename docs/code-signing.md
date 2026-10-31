# Code signing and Smart App Control

For maintainers. Why Windows 11's Smart App Control blocks HellForge, what signing would and would not fix, and the
options. Nothing is bought or configured yet: no release so far has been signed.

## What players see

Smart App Control (Windows Security > App & browser control > Smart App Control settings) refuses to start a program,
or load a DLL, unless Microsoft's cloud service knows it to be safe or it carries a valid signature from a publisher
Windows trusts. It is on by default on clean Windows 11 installs, starts in evaluation mode, and Windows turns it on or
off by itself after a while. Once it is off, Windows can only turn it back on after a reset or reinstall of Windows (newer
Windows 11 builds are said to relax that, but players should not count on it).

HellForge signs nothing, so while Smart App Control is on, Windows can block any of these. Each says
"An Application Control policy has blocked this file" (Windows error 4551, `0x800711C7`), or, for a DLL the game loads,
makes the game exit with `0xC0000428`:

| File | Who starts or loads it | What the player sees since this change |
|---|---|---|
| `app\session\HellForge.Session.exe` (NativeAOT) | the launcher, `HellForge.Cli launch` | "Windows blocked HellForge's session runner." and the full reason |
| `app\backend\HellForge.LocalBackend.exe`, `app\sdk-shims\hellforge_steam_broker.exe`, `app\cli\HellForge.Cli.exe`, `network-block\NetworkProbe.exe` | the session runner | the session fails with "Windows blocked <file> ..." |
| `app\sdk-shims\*.dll` (the PlayFab and Steam stand-ins) | the game | the game exits with `0xC0000428`; the session says what it means, and what to do while Smart App Control is on |
| `app\sdk-shims\hellforge_modloader.dll` | the Steam stand-in, in the game | no mods load (the stand-in loads it silently; the evidence is the CodeIntegrity log below) |
| a mod's DLL | the mod loader, in the game | the mod's line in `modloader.log`: "not loaded (... Windows error 4551, an Application Control policy such as Smart App Control blocked it)" |
| `app\capture\HellForge.CaptureProxy.exe` | the launcher (Sync Inventory) | Windows' one line, unchanged |
| `HellForge.exe`, `app\launcher\HellForge.Launcher.exe`, `HellForge-Setup.exe` | the player, setup | Windows' own block dialog; setup says what happened when it cannot start the launcher |

Setup and the launcher read Smart App Control's state (`HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy`,
`VerifiedAndReputablePolicyState`: 0 off, 1 on, 2 evaluation) and warn while it is on, or evaluating, before anything is
blocked. Each session's `session.log` and `result.json` (`smartAppControl`) say which state it ran in. HellForge never
changes the setting and cannot add an exception: Smart App Control has none.

## What signing fixes, and what it does not

Signed with a certificate that chains to a root in Microsoft's Trusted Root Program, HellForge's own files would pass
Smart App Control's signature check even while its cloud service does not know them yet. Microsoft describes the check
as: when the cloud service cannot make a confident prediction, an app with a valid signature runs. Signing does not
stop the cloud service from calling a file malicious, and the session runner's memory edits of the game look like what
malware does, so a signed build should still be tried with Smart App Control on before a release says it works.

Signing HellForge's builds does not cover files made on the player's PC:

- **Mods built from source** (the mod compiler, Zig, runs on the player's PC) and mod DLLs that mod authors ship unsigned.
  Smart App Control would still block them: a player with it on could play only data and stats mods.
- **`network-block\NetworkProbe.exe`**, which the network block's scripts compile on the player's PC
  (`New-HellForgeBlockProbe`, `scripts/NetworkBlock.Common.ps1`). It would need to ship prebuilt and signed instead.
- **The network block and ownership scripts** (`.ps1`). Windows may run unsigned scripts in Constrained Language mode
  under an App Control policy, where `Add-Type` (the probe's compile) is refused. Not confirmed on a player's PC yet;
  `Sign-Files.ps1` signs the shipped scripts too.

## The options

Prices and eligibility are as Microsoft and the certificate authorities published them in 2025-2026; check them before
deciding.

| Option | Cost | What it takes | Notes |
|---|---|---|---|
| **Azure Trusted Signing** (Microsoft renames it Artifact Signing) | about US$10 a month (Basic) | an Azure subscription and identity validation of the publisher (an organisation, or an individual in the countries Microsoft allows) | Microsoft keeps the key; certificates last days and are renewed for you, so every signature needs a timestamp. Signs from CI with an Azure sign-in. The cheapest and simplest. |
| **OV certificate** from a CA (DigiCert, Sectigo, SSL.com and others) | about US$200-500 a year | validation of an organisation or an individual | Since June 2023 the key must live in a hardware token or a cloud HSM, so CI signing needs the CA's cloud signing service (often an extra fee). |
| **EV certificate** | about US$300-700 a year | stricter organisation validation, hardware key | Since 2024 it no longer gives instant SmartScreen reputation either, so for Smart App Control it buys little over OV. |
| **SignPath Foundation** (free for open-source projects) | free | an accepted open-source project whose release builds come from its public CI | The certificate names SignPath Foundation as the publisher, not HellForge. |

SmartScreen's "Windows protected your PC" prompt for a downloaded setup is a separate check: it goes by reputation, which
a signing identity builds up over downloads, for every option above.

## The pipeline hook

`scripts/Sign-Files.ps1` signs with `signtool` when one of these is set in the environment, and otherwise prints
"Signing skipped" and changes nothing:

- `HELLFORGE_SIGN_DLIB` and `HELLFORGE_SIGN_METADATA`: Azure Trusted Signing's `Azure.CodeSigning.Dlib.dll` and the
  `metadata.json` naming the account and certificate profile.
- `HELLFORGE_SIGN_CERT_SHA1`: a certificate in the Windows certificate store (OV or EV, on a token too), by thumbprint.
- `HELLFORGE_SIGNTOOL` (where `signtool.exe` is) and `HELLFORGE_SIGN_TIMESTAMP` (the timestamp server), when needed.

The build scripts call it at the points where signing does not break a pin: `Build-Engine.ps1` signs the session runner
and backend, the stand-ins, mod loader, WFP tool and broker before `build.json` pins them, and the scripts before
`engine.json` hashes them; `Publish-HellForge.ps1` signs the launcher, CLI, Steam probe, capture proxy, bundled mods and
`HellForge.exe`; `Package-HellForge.ps1` signs `HellForge-Setup.exe`. Files that already carry a valid signature (the .NET
runtime's) are left alone. CI does not sign: it has no certificate, and its builds are not released.

## Evidence from players

When a player reports the block, ask for the CodeIntegrity log entries that name the file: Event Viewer > Applications
and Services Logs > Microsoft > Windows > CodeIntegrity > Operational, events 3077 (blocked), 3033 (a DLL did not meet
the signing level) and 3089 (the signature details that go with them). In PowerShell:

```powershell
Get-WinEvent -LogName 'Microsoft-Windows-CodeIntegrity/Operational' -MaxEvents 200 |
    Where-Object Id -in 3033,3077,3089 | Format-List TimeCreated,Id,Message
```

With the session's `session.log` (its `Smart App Control:` line) that shows which files were blocked and in which state.
