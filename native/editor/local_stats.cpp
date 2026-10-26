/* The Local Stats file (local_stats.h), read and written with json.h, which keeps everything it does not change as it
 * was (numbers keep their text). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <map>
#include <mutex>
#include "json.h"
#include "local_stats.h"

using hfjson::Value;

namespace
{
struct Change { std::string category, target, key; bool remove; double value; bool whole; };
std::mutex g_lock;
std::map<std::string, Change> g_pending; /* by category, target and key: the latest change of each field */
Value g_saved;                           /* the file as last read or written */
Value g_modSaved;                        /* the mod content file (mod-stats.json), as last read or written */
std::wstring g_modPath;
std::wstring g_sandboxFolder;
std::map<std::string, Change> g_journal; /* sandboxed: every change saved this session, the latest of each field */
unsigned g_version = 1;
bool g_hostShown;                        /* joined: Get and Entries show the host's */
Value g_hostSaved, g_hostModSaved;       /* the host's Local Stats file and mod content values */

bool ModCategory(const std::string& category) { return category.rfind("mod:", 0) == 0; }
Value& DocumentOf(const std::string& category) { return ModCategory(category) ? g_modSaved : g_saved; }
const Value& ShownOf(const std::string& category)
{
    if (!g_hostShown) return DocumentOf(category);
    return ModCategory(category) ? g_hostModSaved : g_hostSaved;
}

/* A document's numbers as entries (category, target, key, value), in its order. */
void AddEntries(const Value& document, std::vector<localstats::Entry>& out)
{
    for (auto& category : document.members)
    {
        if (category.second.kind != Value::Object) continue;
        for (auto& target : category.second.members)
            for (auto& field : target.second.members)
                if (field.second.kind == Value::Number) out.push_back({category.first, target.first, field.first, field.second.AsNumber()});
    }
}

std::vector<localstats::Entry> EntriesOf(std::initializer_list<const Value*> documents)
{
    std::vector<localstats::Entry> out;
    for (const Value* document : documents) AddEntries(*document, out);
    return out;
}

/* A host's file as the backend wrote it ("" none): an object, or empty when it is not one. */
Value HostDocument(const std::string& text)
{
    Value document;
    std::string why;
    if (text.empty() || !hfjson::Parse(text, &document, &why) || document.kind != Value::Object)
    {
        document.kind = Value::Object;
        document.members.clear();
    }
    return document;
}

void ReadModDocument(Value* document)
{
    document->kind = Value::Object;
    document->members.clear();
    std::string text, why;
    if (!g_modPath.empty() && hfjson::ReadFile(g_modPath, &text) && (!hfjson::Parse(text, document, &why) || document->kind != Value::Object))
    {
        document->kind = Value::Object;
        document->members.clear();
    }
    if (!document->Find("format")) document->members.insert(document->members.begin(), {"format", Value::MakeNumber(1, true)});
}

std::string Id(const std::string& category, const std::string& target, const std::string& key) { return category + '\t' + target + '\t' + key; }

void Apply(Value& document, const Change& c)
{
    if (c.remove)
    {
        Value* category = document.Find(c.category);
        Value* target = category ? category->Find(c.target) : nullptr;
        if (target == nullptr) return;
        target->Erase(c.key);
        if (target->members.empty()) category->Erase(c.target);
        if (category->members.empty()) document.Erase(c.category);
        return;
    }
    Value value = Value::MakeNumber(c.value, c.whole);
    Value& target = document.At(c.category).At(c.target);
    if (Value* old = target.Find(c.key)) *old = value; else target.members.emplace_back(c.key, value);
}

bool ReadDocument(const std::wstring& folder, Value* document, bool* existed, std::string* error)
{
    std::wstring path = folder + L"\\hellforge-stats.json", manifest = folder + L"\\hellforge-mod.json";
    document->kind = Value::Object;
    document->members.clear();
    std::string text;
    *existed = GetFileAttributesW(folder.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (*existed && GetFileAttributesW(manifest.c_str()) == INVALID_FILE_ATTRIBUTES) { *error = "mods/zz.local.stats exists without its manifest"; return false; }
    if (*existed && hfjson::ReadFile(path, &text))
    {
        std::string why;
        if (!hfjson::Parse(text, document, &why) || document->kind != Value::Object) { *error = "the Local Stats file is not valid JSON; fix or remove it"; return false; }
    }
    if (!document->Find("format")) document->members.insert(document->members.begin(), {"format", Value::MakeNumber(1, true)});
    return true;
}
/* Sandboxed (low integrity, which cannot write to mods/): the session's saved changes, as a list the backend applies to
 * the Local Stats file and the editor's mod-stats.json once the game is gone (SandboxStats.cs), checking each. */
std::string JournalText(const std::map<std::string, Change>& journal)
{
    Value root, list;
    root.kind = Value::Object;
    list.kind = Value::Array;
    root.members.emplace_back("format", Value::MakeNumber(1, true));
    for (auto& entry : journal)
    {
        const Change& c = entry.second;
        Value item;
        item.kind = Value::Object;
        item.members.emplace_back("category", Value::MakeString(c.category));
        item.members.emplace_back("target", Value::MakeString(c.target));
        item.members.emplace_back("key", Value::MakeString(c.key));
        if (c.remove) item.members.emplace_back("remove", Value::MakeBool(true));
        else item.members.emplace_back("value", Value::MakeNumber(c.value, c.whole));
        list.items.push_back(item);
    }
    root.members.emplace_back("changes", list);
    return hfjson::Write(root);
}

/* Changes back in the queue, tried again later; newer changes win. g_lock is held. */
void Requeue(const std::map<std::string, Change>& changes)
{
    for (auto& c : changes) g_pending.emplace(c.first, c.second);
}

/* Sandboxed: what is shown stays as Set and Remove left it (the files in mods/ are not written). */
bool SaveSandboxed(const std::wstring& sandbox, const std::map<std::string, Change>& changes, const std::map<std::string, Change>& modChanges,
    std::string* error)
{
    std::map<std::string, Change> journal;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        journal = g_journal;
    }
    for (auto& c : modChanges) journal[c.first] = c.second;
    for (auto& c : changes) journal[c.first] = c.second;
    if (hfjson::WriteAtomically(sandbox + L"\\changes.json", JournalText(journal)))
    {
        std::lock_guard<std::mutex> hold(g_lock);
        g_journal.swap(journal);
        return true;
    }
    std::lock_guard<std::mutex> hold(g_lock);
    Requeue(modChanges);
    Requeue(changes);
    *error = "the session's changes could not be written";
    return false;
}

/* The editor's own file: the file as it is, the changes, written whole. */
bool SaveModFile(const std::map<std::string, Change>& changes, const std::map<std::string, Change>& modChanges, std::string* error)
{
    Value document;
    ReadModDocument(&document);
    for (auto& entry : modChanges) Apply(document, entry.second);
    bool written = !g_modPath.empty() && hfjson::WriteAtomically(g_modPath, hfjson::Write(document));
    std::lock_guard<std::mutex> hold(g_lock);
    if (!written)
    {
        Requeue(modChanges);
        Requeue(changes);
        *error = "the editor's mod-stats.json could not be written";
        return false;
    }
    g_modSaved = document;
    for (auto& entry : g_pending) if (ModCategory(entry.second.category)) Apply(g_modSaved, entry.second);
    g_version++;
    return true;
}

/* The changed document written into the mod's folder, or the mod removed when nothing is left; the reason it failed,
 * or nullptr. */
const char* WriteStats(const std::wstring& folder, const std::wstring& path, const std::wstring& manifest, const Value& document, bool existed)
{
    if (document.members.size() == 1)
    {
        /* Nothing left: the mod goes, as when the launcher's editor undoes every change. A file another program has open
         * stays (Windows refuses the delete): tried again later, not taken as saved with the change still in it. */
        if (existed && !DeleteFileW(path.c_str()) && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            return "the Local Stats file could not be removed (another program has it open?)";
        if (existed) { DeleteFileW(manifest.c_str()); RemoveDirectoryW(folder.c_str()); }
        return nullptr;
    }
    CreateDirectoryW(folder.c_str(), nullptr);
    if (!hfjson::WriteAtomically(path, hfjson::Write(document))) return "the Local Stats file could not be written";
    if (GetFileAttributesW(manifest.c_str()) == INVALID_FILE_ATTRIBUTES &&
        !hfjson::WriteAtomically(manifest, "{\n  \"format\": 1,\n  \"id\": \"zz.local.stats\",\n  \"name\": \"Local Stats\",\n  \"version\": \"1.0.0\",\n"
            "  \"description\": \"Stats saved by the HellForge arsenal editor.\",\n  \"gameplay\": true,\n  \"editor\": \"hellforge-stats-editor-v1\"\n}\n"))
        return "the Local Stats manifest could not be written";
    return nullptr;
}

/* The Local Stats file: the file as it is, the changes, written whole, under the launcher's editor's lock. */
bool SaveStats(const std::string& modsFolder, const std::map<std::string, Change>& changes, std::string* error)
{
    std::wstring mods = hfjson::Wide(modsFolder), folder = mods + L"\\zz.local.stats", path = folder + L"\\hellforge-stats.json",
        manifest = folder + L"\\hellforge-mod.json";
    /* The launcher's editor holds this lock while it saves; wait for it. */
    HANDLE lock = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 40 && lock == INVALID_HANDLE_VALUE; attempt++)
    {
        lock = CreateFileW((mods + L"\\.hellforge-stats-editor.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lock == INVALID_HANDLE_VALUE) Sleep(50);
    }
    auto fail = [&](const char* why) {
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        std::lock_guard<std::mutex> hold(g_lock);
        Requeue(changes); /* tried again later; newer changes win */
        *error = why;
        return false;
    };
    if (lock == INVALID_HANDLE_VALUE) return fail("the stats file is locked");

    Value document;
    bool existed;
    std::string why;
    if (!ReadDocument(folder, &document, &existed, &why)) { std::string copy = why; return fail(copy.c_str()); }
    for (auto& entry : changes) Apply(document, entry.second);
    if (const char* refused = WriteStats(folder, path, manifest, document, existed)) return fail(refused);
    CloseHandle(lock);
    /* What the file holds now, with any change queued while it was written. */
    std::lock_guard<std::mutex> hold(g_lock);
    g_saved = document;
    for (auto& entry : g_pending) if (!ModCategory(entry.second.category)) Apply(g_saved, entry.second);
    g_version++;
    return true;
}
} // namespace

namespace localstats
{
void SetSandboxSaveFolder(const std::wstring& folder)
{
    std::lock_guard<std::mutex> hold(g_lock);
    g_sandboxFolder = folder;
}

std::wstring SandboxFolder()
{
    std::lock_guard<std::mutex> hold(g_lock);
    return g_sandboxFolder;
}

void SetModFile(const std::wstring& path)
{
    std::lock_guard<std::mutex> hold(g_lock);
    g_modPath = path;
}

bool IsMod(const std::string& category) { return ModCategory(category); }

bool Load(const std::string& modsFolder, std::string* error)
{
    Value document, mods;
    bool existed;
    ReadModDocument(&mods);
    {
        std::lock_guard<std::mutex> hold(g_lock);
        g_modSaved = mods;
        g_version++;
    }
    if (!ReadDocument(hfjson::Wide(modsFolder) + L"\\zz.local.stats", &document, &existed, error)) return false;
    std::lock_guard<std::mutex> hold(g_lock);
    g_saved = document;
    g_version++;
    return true;
}

void Set(const std::string& category, const std::string& target, const std::string& key, double value, bool whole)
{
    std::lock_guard<std::mutex> hold(g_lock);
    Change c{category, target, key, false, value, whole};
    g_pending[Id(category, target, key)] = c;
    Value& document = DocumentOf(category);
    if (document.kind != Value::Object) document.kind = Value::Object;
    Apply(document, c);
    g_version++;
}

void Remove(const std::string& category, const std::string& target, const std::string& key)
{
    std::lock_guard<std::mutex> hold(g_lock);
    Change c{category, target, key, true, 0, false};
    g_pending[Id(category, target, key)] = c;
    Apply(DocumentOf(category), c);
    g_version++;
}

bool Get(const std::string& category, const std::string& target, const std::string& key, double* value)
{
    std::lock_guard<std::mutex> hold(g_lock);
    const Value* c = ShownOf(category).Find(category);
    const Value* t = c ? c->Find(target) : nullptr;
    const Value* v = t ? t->Find(key) : nullptr;
    if (v == nullptr || v->kind != Value::Number) return false;
    *value = v->AsNumber();
    return true;
}

std::vector<Entry> Entries()
{
    std::lock_guard<std::mutex> hold(g_lock);
    return g_hostShown ? EntriesOf({&g_hostSaved, &g_hostModSaved}) : EntriesOf({&g_saved, &g_modSaved});
}

std::vector<Entry> OwnEntries()
{
    std::lock_guard<std::mutex> hold(g_lock);
    return EntriesOf({&g_saved, &g_modSaved});
}

void ShowHost(const std::string& stats, const std::string& mods)
{
    Value document = HostDocument(stats), modDocument = HostDocument(mods);
    std::lock_guard<std::mutex> hold(g_lock);
    g_hostSaved = document;
    g_hostModSaved = modDocument;
    g_hostShown = true;
    g_version++;
}

void ShowOwn()
{
    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_hostShown) return;
    g_hostShown = false;
    g_hostSaved = Value();
    g_hostModSaved = Value();
    g_version++;
}

bool ShowingHost()
{
    std::lock_guard<std::mutex> hold(g_lock);
    return g_hostShown;
}

unsigned Version()
{
    std::lock_guard<std::mutex> hold(g_lock);
    return g_version;
}

bool Pending()
{
    std::lock_guard<std::mutex> hold(g_lock);
    return !g_pending.empty();
}

bool Save(const std::string& modsFolder, std::string* error)
{
    std::map<std::string, Change> changes, modChanges;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        changes.swap(g_pending);
    }
    for (auto it = changes.begin(); it != changes.end();)
        if (ModCategory(it->second.category)) { modChanges.insert(*it); it = changes.erase(it); }
        else ++it;
    std::wstring sandbox;
    { std::lock_guard<std::mutex> hold(g_lock); sandbox = g_sandboxFolder; }
    if (!sandbox.empty()) return SaveSandboxed(sandbox, changes, modChanges, error);
    if (!modChanges.empty() && !SaveModFile(changes, modChanges, error)) return false;
    if (changes.empty()) return true;
    return SaveStats(modsFolder, changes, error);
}
} // namespace localstats
