/* The Local Stats mod (mods/zz.local.stats), which the launcher's STATS page saves into as well: the in-game editor keeps
 * its changes there, so the stats pass applies them whenever the game loads, from the next boot on. */
#pragma once
#include <string>
#include <vector>

namespace localstats
{
struct Entry { std::string category, target, key; double value; };

// Content mods added (a custom stratagem) is not in HellForge's stats catalogue, so the stats pass could not read its
// entries: categories that start with "mod:" are kept in a file of the editor's own instead (mod-stats.json in its
// folder), which the editor applies itself at every session's start (live edits). Set before Load.
void SetModFile(const std::wstring& path);
bool IsMod(const std::string& category);

// Sandboxed (low integrity), the game cannot write to mods/: Save writes every change saved this session as a list,
// changes.json in this folder, which HellForge applies to the files once the game is gone (SandboxStats.cs,
// docs/steam-bridge.md); the files in mods/ are only read. Set before Load; empty disables.
void SetSandboxSaveFolder(const std::wstring& folder);
std::wstring SandboxFolder();

// Reads the file as it is now (the editor shows what it holds). False with *error when it cannot be read.
bool Load(const std::string& modsFolder, std::string* error);
// Queues a field's value (a stats-file key of a target in a category: "weapons", "AR-23 Liberator", "weapon.fire_rate"),
// or its removal; Save writes every queued change. What Get and Entries show includes queued changes.
void Set(const std::string& category, const std::string& target, const std::string& key, double value, bool whole);
void Remove(const std::string& category, const std::string& target, const std::string& key);
bool Get(const std::string& category, const std::string& target, const std::string& key, double* value);
std::vector<Entry> Entries();
// Joined to a friend's lobby: what Get and Entries show are the host's saved changes instead (its Local Stats file and
// mod content values, as the session's backend writes them beside the editor: host-stats.json, host-mod-stats.json),
// until ShowOwn. Set, Remove and Save still work on this machine's own files.
void ShowHost(const std::string& stats, const std::string& mods);
void ShowOwn();
bool ShowingHost();
// This machine's own saved (and queued) changes, whichever are shown.
std::vector<Entry> OwnEntries();
// Changes when what Get and Entries show changes.
unsigned Version();
bool Pending();
// Reads the file again (the launcher may have changed it), applies the queued changes, and writes it atomically under the
// launcher's lock, making the mod if it is missing and removing it when nothing is left. Returns false with *error set.
bool Save(const std::string& modsFolder, std::string* error);
}
