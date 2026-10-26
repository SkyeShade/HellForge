/* The editor's window (app_internal.h): staging, applying and editing values, the mods each row's value is held by,
 * and the marks of objects, categories and headers. */
#include <math.h>
#include <stdlib.h>
#include <algorithm>
#include "app_internal.h"

namespace editorapp
{
/* ---- staging and applying ---- */

/* The trait slot an AP pick goes to: the one holding an armor penetration trait, else (a new one) the first empty one. */
static const Row* ApSlot(const Row* row, double value)
{
    auto traits = TraitRows(row);
    for (const Row* t : traits) if (double v = ViewOf(t).value; v != 0 && row->OptionOf(v)) return t;
    if (value == 0) return nullptr;
    for (const Row* t : traits) if (ViewOf(t).value == 0) return t;
    return nullptr;
}

/* a donor projectile: the weapon's own round is not what it fires any more, so its pending values go */
static void DropOwnRound(const Row* row, double value)
{
    int dropped = 0;
    for (const Row* r : row->object->rows)
        if (r->fires == row->key && r->target == row->target && g_pending.erase(r)) dropped++;
    if (dropped) Toast(std::to_string(dropped) + " pending change" + (dropped == 1 ? "" : "s") + " to its own round dropped: it fires " +
        RefLabel(row, value) + " now", PENDING);
}

void Stage(const Row* row, double value)
{
    if (RefuseLocked()) return;
    if (IsApLabel(row))
    {
        if (const Row* slot = ApSlot(row, value)) Stage(slot, value);
        else if (value != 0) Toast("Every trait slot holds a trait: empty one first, then pick the armor penetration", ERROR_);
        return;
    }
    for (const Row* s : g_catalog.Siblings(row)) if (s != row) g_pending.erase(s);
    RowView applied = ViewOf(row, false);
    if (applied.has && Same(row, value, applied.value)) g_pending.erase(row);
    else g_pending[row] = {value, false};
    g_markVersion++;
    if (row->pick == "projectile" && applied.hasBase && !Same(row, value, applied.base)) DropOwnRound(row, value);
}

/* Stages a field's reset: it shows its default now, and goes back to it on APPLY (DISCARD takes it back). Only a field
 * Local Stats changed has anything to reset. */
bool StageReset(const Row* row)
{
    if (RefuseLocked()) return false;
    if (IsApLabel(row)) { const Row* slot = ApSlot(row, 0); return slot != nullptr && StageReset(slot); }
    double saved;
    for (const Row* s : g_catalog.Siblings(row)) g_pending.erase(s);
    g_markVersion++;
    if (!Saved(row, &saved)) return false;
    g_pending[row] = {0, true};
    return true;
}

static bool HasCopyRow() { return g_host.api->size >= offsetof(HfApi, copy_row) + sizeof(void*) && g_host.api->copy_row != nullptr; }

/* The option the arm swaps shown hold for the row (the host's while joined), or nullptr. */
static const Row::Option* ChosenOption(const Row* row)
{
    auto chosen = Mounts().find(MountKey(row));
    return chosen != Mounts().end() ? row->OptionOf(chosen->second) : nullptr;
}

/* another explosion's settings over the row (SDK 9 copy_row), or its own back */
static void SendRowCopy(const Row* row)
{
    const Row::Option* o = ChosenOption(row);
    uint64_t from = o == nullptr || o == &row->options[0] ? 0 : (uint64_t)o->value;
    if (!HasCopyRow() || g_host.api->copy_row(g_host.mod, row->component.c_str(), row->ref, from, row->offset, row->size) == 0)
        SetError(row, HasCopyRow() ? Refusal("HellForge refused the copy") : "this HellForge has no copy_row");
    else ClearError(row);
}

/* A mount slot's or hellpod row's choice to the game (its option's entity, or a count), or back to its vanilla one. */
void SendMount(const Row* row)
{
    if (row->choice == "rowcopy") { SendRowCopy(row); return; }
    bool wide = row->type == HF_TYPE_U64;
    HfField field{row->component.c_str(), g_layout, (uint16_t)row->offset, (uint16_t)(wide ? 8 : 4), row->type, 0};
    const Row::Option* o = ChosenOption(row);
    int applies;
    if (o == nullptr || o == &row->options[0]) applies = g_host.api->set_live(g_host.mod, row->ref, &field, nullptr, row->shared ? HF_LIVE_SHARED : 0);
    else
    {
        HfValue v = wide ? HfValue{HF_TYPE_U64, 0, o->resource} : HfValue{row->type, o->value, 0};
        applies = g_host.api->set_live(g_host.mod, row->ref, &field, &v, row->shared ? HF_LIVE_SHARED : 0);
    }
    if (applies == 0) SetError(row, Refusal("HellForge refused the swap"));
    else ClearError(row);
}

/* Back to the base: out of Local Stats (the row and every row on its bytes), and the base back in the game. */
static void ResetRow(const Row* row)
{
    if (IsMount(row))
    {
        g_mounts.erase(MountKey(row));
        SendMount(row);
        SaveSettings();
        g_pending.erase(row);
        g_markVersion++;
        return;
    }
    if (!row->choice.empty())
    {
        g_choice[ChoiceIndex(row)] = -1;
        SendPassives();
        SaveSettings();
        g_pending.erase(row);
        g_markVersion++;
        return;
    }
    for (const Row* s : g_catalog.Siblings(row))
    {
        double v;
        if (localstats::Get(s->category, s->target, s->key, &v)) localstats::Remove(s->category, s->target, s->key);
        if (s->Live())
        {
            HfField field = FieldOf(s);
            g_host.api->set_live(g_host.mod, s->ref, &field, nullptr, HF_LIVE_BASE);
        }
        else if (LiveStat(s))
        {
            HfStat stat = StatOf(s);
            g_host.api->set_stat(g_host.mod, &stat, nullptr, HF_LIVE_BASE);
        }
        ClearError(s);
        g_pending.erase(s);
        g_live[s].at = 0;
    }
    g_markVersion++;
}

/* An arm swap, pod or row copy kept in the settings (its first option, the vehicle's own, is not kept) and sent. */
static void ApplyMount(const Row* row, double value)
{
    if (!row->options.empty() && value == row->options[0].value) g_mounts.erase(MountKey(row));
    else g_mounts[MountKey(row)] = value;
    SendMount(row);
    SaveSettings();
}

/* A value to the running game: set_live, or set_stat for a field the stats pass writes at boot. */
static void SendValue(const Row* row, double value)
{
    if (row->Live())
    {
        HfField field = FieldOf(row);
        HfValue v{row->type, value, 0};
        int applies = g_host.api->set_live(g_host.mod, row->ref, &field, &v, row->shared ? HF_LIVE_SHARED : 0);
        if (applies == 0) SetError(row, Refusal("HellForge refused it"));
        else ClearError(row);
    }
    else if (LiveStat(row))
    {
        HfStat stat = StatOf(row);
        HfValue v{row->type, value, 0};
        if (g_host.api->set_stat(g_host.mod, &stat, &v, 0) == 0) SetError(row, Refusal("HellForge refused it (saved for the next boot)"));
        else ClearError(row);
    }
}

static void ApplyRow(const Row* row, double value)
{
    if (IsMount(row)) { ApplyMount(row, value); return; }
    if (!row->choice.empty())
    {
        g_choice[ChoiceIndex(row)] = value;
        SendPassives();
        SaveSettings();
        return;
    }
    LiveState& live = Live(row, true);
    if ((row->Live() || row->Stat()) && live.read && Same(row, value, live.base)) { ResetRow(row); return; }
    for (const Row* s : g_catalog.Siblings(row))
    {
        double v;
        if (s != row && localstats::Get(s->category, s->target, s->key, &v)) localstats::Remove(s->category, s->target, s->key);
    }
    localstats::Set(row->category, row->target, row->key, row->unlimited && value == 4294967295.0 ? -1 : value, row->Whole());
    SendValue(row, value);
    for (const Row* s : g_catalog.Siblings(row)) g_live[s].at = 0;
}

/* A field's value once the changes are applied: the saved one, else what the game holds. */
static double Effective(const Row* row)
{
    double v;
    if (localstats::Get(row->category, row->target, row->key, &v)) return v;
    const LiveState& s = Live(row, true);
    return s.read ? s.now : 0;
}

/* A weapon's input binding (WeaponData +184 left, +188 right; 2 rate of fire), the field get_live and set_live take. */
static HfField InputField(int input) { return HfField{"WeaponDataComponentData", g_layout, (uint16_t)(input ? 188 : 184), 4, HF_TYPE_U32, 0}; }

/* An input's binding now and its base, read from the game (the catalogue has no row for it). */
static bool ReadInput(const Row* rate, int input, double* now, double* base)
{
    if (!HasGetLive()) return false;
    HfField field = InputField(input);
    HfLiveValue value;
    if (!g_host.api->get_live(g_host.mod, rate->ref, &field, &value)) return false;
    *now = value.now.number;
    *base = value.base.number;
    return true;
}

/* An input to rate of fire, or back to its base (bind false), in the game and in Local Stats. */
static void SetInput(const Row* rate, int input, bool bind)
{
    const char* key = input ? "weapon_function.right" : "weapon_function.left";
    HfField field = InputField(input);
    HfValue v{HF_TYPE_U32, 2, 0};
    g_host.api->set_live(g_host.mod, rate->ref, &field, bind ? &v : nullptr, bind ? 0 : HF_LIVE_BASE);
    if (bind) localstats::Set(rate->category, rate->target, key, 2, true);
    else localstats::Remove(rate->category, rate->target, key);
}

/* More than one rate of fire (rate slots X and Z, ProjectileWeapon +4 / +12, beside Y) is selectable only while an input
 * is bound to rate of fire (WeaponData +184 / +188 = 2: the press dispatcher reaches the rate cycle only for it,
 * game.dll 0x75541F; the cycle skips empty slots, 0x617A0B; HD2Mods research\coop-weapon-stats). The editor sets it
 * itself, as the HD2R Editor does: a weapon given more than one rate gets a free input bound (the left, else the right)
 * unless one is bound already (the MG-206's right input, say); a weapon back to one rate loses the binding the editor
 * gave it. Returns what it says to the player: only when it cannot, or when two rates are the same. */
/* A free input (the left, else the right) bound to rate of fire, unless one is bound already: "" when it is, else why
 * not. known, now: the inputs read (ReadInput). */
static std::string BindFreeInput(const Row* rate, const bool known[2], const double now[2])
{
    if (now[0] == 2 || (known[1] && now[1] == 2)) return "";
    int input = now[0] == 0 ? 0 : known[1] && now[1] == 0 ? 1 : -1;
    if (input < 0) return "Both of the weapon's inputs are in use, so its other rates of fire can't be selected";
    SetInput(rate, input, true);
    return "";
}

/* Whether two of the rates of fire are the same (X, Y, Z; 0 an empty slot). */
static bool SameRates(double xv, double yv, double zv) { return (xv != 0 && (xv == yv || xv == zv)) || (zv != 0 && zv == yv); }

static std::string BindRateInput(const Row* rate)
{
    auto entry = [&](const char* key) { return g_catalog.Entry(rate->category, rate->target, key); };
    const Row *x = entry("fire_rate.modes.x"), *y = entry("weapon.fire_rate"), *z = entry("fire_rate.modes.z");
    double xv = x ? Effective(x) : 0, zv = z ? Effective(z) : 0, yv = y ? Effective(y) : 0;
    double now[2], base[2];
    bool known[2] = {ReadInput(rate, 0, &now[0], &base[0]), ReadInput(rate, 1, &now[1], &base[1])};
    if (!known[0]) return ""; /* no input to bind on this weapon */
    if (xv == 0 && zv == 0) /* one rate: the binding the editor gave goes */
    {
        for (int i = 0; i < 2; i++) if (known[i] && now[i] == 2 && base[i] != 2) SetInput(rate, i, false);
        return "";
    }
    if (std::string why = BindFreeInput(rate, known, now); !why.empty()) return why;
    return SameRates(xv, yv, zv) ? "Two rates of fire are the same, so a press may seem to do nothing" : "";
}

/* Rate of fire bound for each weapon whose X or Z rate was applied (BindRateInput): the last thing that says. */
static std::string BindRates(const std::vector<std::pair<const Row*, Pend>>& pending)
{
    std::string rates;
    std::set<std::string> weapons;
    for (auto& p : pending)
        if ((p.first->key == "fire_rate.modes.x" || p.first->key == "fire_rate.modes.z") && weapons.insert(p.first->category + "/" + p.first->target).second)
            if (std::string said = BindRateInput(p.first); !said.empty()) rates = said;
    return rates;
}

/* What Apply says: the rates' note, else how many changes were applied, else how many were refused. */
static void ToastApplied(int n, const std::string& rates)
{
    if (!rates.empty() && g_errors.empty()) Toast(rates, PENDING);
    else Toast(g_errors.empty() ? (std::to_string(n) + (n == 1 ? " change applied" : " changes applied") + " and saved to Local Stats") :
        std::to_string(g_errors.size()) + " refused: see the ERRORS badge", g_errors.empty() ? OK : ERROR_);
}

void Apply()
{
    if (RefuseLocked()) return;
    if (g_pending.empty()) return;
    int n = (int)g_pending.size();
    std::vector<std::pair<const Row*, Pend>> pending(g_pending.begin(), g_pending.end());
    g_pending.clear();
    /* status slots are filled from slot 1 and emptied from the last, so no write leaves a gap */
    auto rank = [](const std::pair<const Row*, Pend>& p) {
        const Row* r = p.first;
        if (r->pick != "status") return 0;
        double to = p.second.reset ? ViewOf(r, false).base : p.second.value;
        return to != 0 ? 1 + r->slot : 10 - r->slot;
    };
    std::stable_sort(pending.begin(), pending.end(), [&](auto& a, auto& b) { return rank(a) < rank(b); });
    for (auto& p : pending) { if (p.second.reset) ResetRow(p.first); else ApplyRow(p.first, p.second.value); }
    std::string rates = BindRates(pending);
    g_markVersion++;
    g_host.saveSoon(800);
    Sound(sounds::APPLY);
    ToastApplied(n, rates);
}

void Discard() { g_pending.clear(); g_markVersion++; }

std::vector<const Row*> g_choiceRows; /* the rows the editor keeps itself (armor perks, arm swaps): found at Init */

const std::vector<const Row*>& SavedRows()
{
    static unsigned stats, version;
    static std::vector<const Row*> out;
    unsigned now = localstats::Version();
    if (stats == now && version == g_markVersion && !out.empty()) return out;
    stats = now; version = g_markVersion;
    out.clear();
    for (auto& e : localstats::Entries()) /* joined: the host's */
        if (const Row* row = g_catalog.Entry(e.category, e.target, e.key)) out.push_back(row);
    for (const Row* row : g_choiceRows) if (ChoiceSaved(row)) out.push_back(row);
    return out;
}

/* RESET TO DEFAULTS, REVERT ALL: every field Local Stats changed staged back to its default, other pending changes
 * dropped; APPLY writes them, DISCARD takes them back. */
void StageResetAll()
{
    if (RefuseLocked()) return;
    g_pending.clear();
    int n = 0;
    for (const Row* row : SavedRows()) if (!PendingOf(row) && StageReset(row)) n++;
    g_markVersion++;
    Toast(n ? std::to_string(n) + (n == 1 ? " reset" : " resets") + " pending: APPLY writes them, DISCARD takes them back" : "Nothing to reset", TEXT);
}

/* ---- holders: the mod whose value each catalogue row holds (get_live / get_stat name it), read a few rows a frame so
 * the objects' and categories' marks and the MODS view know them without stalling the game: every frame from the
 * session's start, open or not (Tick: 0.5 ms of reads; while the editor draws, what the page's reads leave of their 1.5 ms), a pass over the catalogue
 * every 10 s. What a pass finds shows in the marks within half a second. ---- */
std::map<const Row*, std::string> g_holders;
static size_t g_holderNext;
static ULONGLONG g_holderPass, g_holderMarked;
static bool g_holderChanged;
bool g_holdersRead;

/* The mod a row's value is held by now (get_live / get_stat name it), into the holders. */
static void NoteHolder(const Row* row)
{
    const LiveState& live = Live(row, false, false);
    std::string holder = live.read ? live.mod : "";
    auto found = g_holders.find(row);
    if (holder.empty() && found != g_holders.end()) { g_holders.erase(found); g_holderChanged = true; }
    else if (!holder.empty() && (found == g_holders.end() || found->second != holder)) { g_holders[row] = holder; g_holderChanged = true; }
}

void HolderTick(LONGLONG microseconds)
{
    if (!g_catalogOk) return;
    ULONGLONG tick = GetTickCount64();
    if (g_holderChanged && (g_holderNext >= g_catalog.rows.size() || tick - g_holderMarked >= 500))
    { g_markVersion++; g_holderChanged = false; g_holderMarked = tick; }
    if (g_holderNext >= g_catalog.rows.size())
    {
        g_holdersRead = true;
        if (g_holderPass != 0 && tick - g_holderPass < 10000) return;
        g_holderNext = 0;
        g_holderPass = tick;
    }
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    const LONGLONG budget = frequency.QuadPart * microseconds / 1000000;
    while (g_holderNext < g_catalog.rows.size())
    {
        const Row* row = &g_catalog.rows[g_holderNext++];
        if (!row->choice.empty() || IsApLabel(row) || (!row->Live() && !row->Stat())) continue;
        NoteHolder(row);
        QueryPerformanceCounter(&now);
        if (now.QuadPart - start.QuadPart > budget) break;
    }
}

/* ---- markers (what an object, a category or a header holds) ---- */
static MarkCache g_marks;

/* values a mod holds: its mark (a Lua mod's purple) on every object and category showing them */
static void MarkHolders()
{
    for (auto& [row, holder] : g_holders)
        for (const Row* s : g_catalog.Siblings(row))
        {
            bool lua = IsLuaHolder(holder);
            Marks& o = g_marks.objects[s->object];
            Marks& n = g_marks.navs[s->object->category];
            (lua ? o.lua : o.mod) = true;
            (lua ? n.lua : n.mod) = true;
        }
}

const MarkCache& Markers()
{
    unsigned stats = localstats::Version();
    if (g_marks.version == g_markVersion && g_marks.stats == stats && g_marks.errors == g_errorsVersion) return g_marks;
    g_marks.objects.clear();
    g_marks.navs.clear();
    auto mark = [](const Row* row, int kind) {
        for (const Row* s : g_catalog.Siblings(row))
        {
            Marks& o = g_marks.objects[s->object];
            Marks& n = g_marks.navs[s->object->category];
            if (kind == 0) o.error = n.error = true;
            if (kind == 1) o.pending = n.pending = true;
            if (kind == 2) o.editor = n.editor = true;
        }
    };
    /* content a mod added (a custom stratagem): the mod's mark on it and its category, as on its fields */
    for (auto& o : g_catalog.objects)
        if (o->id.rfind("mod/", 0) == 0) { g_marks.objects[o.get()].mod = true; g_marks.navs[o->category].mod = true; }
    MarkHolders();
    for (auto& e : g_errors) { g_marks.objects[e.first->object].error = true; g_marks.navs[e.first->object->category].error = true; }
    for (auto& p : g_pending) mark(p.first, 1);
    for (const Row* row : SavedRows()) mark(row, 2);
    g_marks.version = g_markVersion;
    g_marks.stats = stats;
    g_marks.errors = g_errorsVersion;
    return g_marks;
}

/* The mod holding a row's value: as last read (get_live), else as the holders' pass found it; "" none. */
static std::string HolderOf(const Row* row)
{
    auto live = g_live.find(row);
    std::string holder = live != g_live.end() ? live->second.mod : "";
    if (holder.empty()) { auto h = g_holders.find(row); if (h != g_holders.end()) holder = h->second; }
    return holder;
}

Marks RowsMarks(const std::vector<const Row*>& rows)
{
    Marks m;
    for (const Row* row : rows)
    {
        double v;
        if (g_errors.count(row)) m.error = true;
        if (PendingOf(row)) m.pending = true;
        if (Saved(row, &v)) m.editor = true;
        std::string holder = HolderOf(row);
        if (!holder.empty()) (IsLuaHolder(holder) ? m.lua : m.mod) = true;
    }
    return m;
}

/* ---- editing a value ---- */
/* Whether the picker's filter (lower case) matches the text (an empty one matches everything). */
static bool Matches(const std::string& filter, const std::string& text) { return filter.empty() || Lower(text).find(filter) != std::string::npos; }

/* A choice row's options, under their groups' headings (a pod's pickups by kind). */
static void FillChoices(const Row* row, const std::string& filter, std::vector<PickEntry>& out)
{
    std::string group;
    for (auto& o : row->options)
    {
        if (!Matches(filter, o.label + " " + o.group + " " + o.note)) continue;
        if (!o.group.empty() && o.group != group) { group = o.group; out.push_back({true, 0, group}); } /* a pod's pickups by kind */
        out.push_back({false, o.value, o.label, o.note, o.icon});
    }
}

/* A field with named values: its options, the game's own one said. */
static void FillValues(const Row* row, const std::string& filter, std::vector<PickEntry>& out)
{
    RowView v = ViewOf(row, false);
    for (auto& o : row->options)
        if (Matches(filter, o.label)) out.push_back({false, o.value, o.label, v.hasBase && o.value == v.base ? "the game's own" : ""});
}

/* The catalogue's references a reference row offers, under their groups' headings: all but its own and those it
 * leaves out. */
static void FillCatalogue(const Row* row, const std::string& filter, double own, std::vector<PickEntry>& out)
{
    auto list = g_catalog.references.find(row->pick);
    if (list == g_catalog.references.end()) return;
    std::string group;
    for (auto& o : list->second)
    {
        if (o.value == own || !Matches(filter, o.label + " " + o.group + " " + o.note)) continue;
        if (std::find(row->exclude.begin(), row->exclude.end(), o.value) != row->exclude.end()) continue;
        if (o.group != group) { group = o.group; out.push_back({true, 0, group}); }
        out.push_back({false, o.value, o.label, o.note, "", o.warnings});
    }
}

/* A reference row's: its own, none (an empty slot), then the catalogue's references. */
static void FillReferences(const Row* row, const std::string& filter, std::vector<PickEntry>& out)
{
    RowView v = ViewOf(row, false);
    double own = v.hasBase ? v.base : v.value;
    std::string what = row->pick == "status" ? "applies" : row->pick == "explosion" ? "sets off" : "fires";
    std::string ownLabel = (row->pick == "projectile" ? "Own projectile: " : row->pick == "explosion" ? "Own explosion: " : "Own status: ") + RefLabel(row, own);
    if (Matches(filter, ownLabel)) out.push_back({false, own, ownLabel, "what it " + what + " in the game's own data"});
    if (row->pick != "projectile" && own != 0 && Matches(filter, row->pick == "status" ? "Empty" : "None"))
        out.push_back({false, 0, row->pick == "status" ? "Empty" : "None", row->pick == "status" ? "an empty slot" : "no explosion"});
    FillCatalogue(row, filter, own, out);
}

void PickerFill()
{
    const Row* row = g_picker.row;
    auto& out = g_picker.entries;
    out.clear();
    std::string filter = Lower(g_picker.filter);
    if (!row->choice.empty()) FillChoices(row, filter, out);
    else if (row->Valued()) FillValues(row, filter, out);
    else FillReferences(row, filter, out);
}

/* Whether `i` is an entry of the list (a filter that matches nothing leaves it empty and the index -1). */
bool PickerHas(int i) { return i >= 0 && i < (int)g_picker.entries.size(); }

/* The next selectable entry from `from` going `step` (headers are skipped), or `from`. */
int PickerStep(int from, int step)
{
    int n = (int)g_picker.entries.size();
    for (int i = from + step; i >= 0 && i < n; i += step) if (!g_picker.entries[i].header) return i;
    return from;
}

void PickerSelect(double value)
{
    g_picker.index = PickerStep(-1, 1);
    for (size_t i = 0; i < g_picker.entries.size(); i++)
        if (!g_picker.entries[i].header && g_picker.entries[i].value == value) { g_picker.index = (int)i; break; }
}

static void OpenPicker(const Row* row)
{
    g_picker.on = true;
    g_picker.row = row;
    g_picker.filter.clear();
    PickerFill();
    PickerSelect(ViewOf(row).value);
    g_scroll["picker"] = std::max(0, g_picker.index - 4);
    g_seen.erase("picker");
}

void PickerChoose(int i)
{
    if (!PickerHas(i) || g_picker.entries[i].header) return;
    const Row* row = g_picker.row;
    double value = g_picker.entries[i].value;
    if (row->pick == "status" && value == 0 && LaterSlotFilled(row))
    { Toast("Empty the later status slots first: the slots fill in order, from slot 1", ERROR_); return; }
    Stage(row, value);
    g_picker.on = false;
    Sound(sounds::CLICK);
}

void BeginEdit(const Row* row, const std::string& text, bool fresh)
{
    if (RefuseLocked()) return;
    std::string lock = LockOf(row), block = SlotBlock(row);
    if (!lock.empty() || !block.empty()) { Toast("Locked for now: " + (lock.empty() ? block : lock), ERROR_); return; }
    if (!row->choice.empty() || !row->pick.empty() || row->Valued()) { OpenPicker(row); return; }
    RowView v = ViewOf(row);
    g_edit.on = true;
    g_edit.row = row;
    g_edit.fresh = fresh && text.empty();
    g_edit.buffer = !text.empty() ? text : v.has ? Format(row, v.value) : "";
}

static bool Parse(const Row* row, const std::string& text, double* out, std::string* error)
{
    std::string t = text;
    if (t.find('.') == std::string::npos && std::count(t.begin(), t.end(), ',') == 1) std::replace(t.begin(), t.end(), ',', '.');
    char* end = nullptr;
    double v = strtod(t.c_str(), &end);
    if (t.empty() || end == t.c_str() || *end != 0 || !isfinite(v)) { *error = "Enter a number."; return false; }
    if (row->unlimited && (v == -1 || v == 0)) { *out = 4294967295.0; return true; } /* -1 or 0: unlimited */
    return Valid(row, v, out, error);
}

/* Why a number does not fit the row's storage, or nullptr. */
static const char* StorageError(const Row* row, double v)
{
    if (row->type == HF_TYPE_U32 && (v < 0 || v > 4294967295.0)) return v < 0 ? "Must not be negative." : "Number is too large.";
    if (row->type == HF_TYPE_I32 && (v < -2147483648.0 || v > 2147483647.0)) return "Number is too large.";
    if (row->type == HF_TYPE_F32 && fabs(v) > 3.4e38) return "Number is too large.";
    if (row->type == HF_TYPE_U8 && (v < 0 || v > 255)) return "Allowed range: 0 to 255.";
    return nullptr;
}

/* A number as the row takes it (whole, in range, fitting its storage), or false with *error. */
bool Valid(const Row* row, double v, double* out, std::string* error)
{
    if (!isfinite(v)) { *error = "Enter a number."; return false; }
    if (row->Whole() && v != floor(v)) { *error = "Whole numbers only."; return false; }
    if ((row->hasMin && v < row->min) || (row->hasMax && v > row->max))
    { *error = "Allowed range: " + Format(row, row->hasMin ? row->min : -INFINITY) + " to " + Format(row, row->max); return false; }
    if (const char* why = StorageError(row, v)) { *error = why; return false; }
    if (row->Valued() && row->OptionOf(v) == nullptr) { *error = "not one of its values"; return false; }
    *out = v;
    return true;
}

void CommitEdit()
{
    if (!g_edit.on) return;
    g_edit.on = false;
    double v;
    std::string error;
    if (g_edit.buffer.empty()) return;
    if (!Parse(g_edit.row, g_edit.buffer, &v, &error)) { Toast(error, ERROR_); return; }
    Stage(g_edit.row, v);
}

/* mission uses (HD2R Editor's): left from 1 is unlimited, left from unlimited is the most (100); false when the nudge is
 * neither of those */
static bool NudgeUses(const Row* row, double value, int direction)
{
    if (value == 4294967295.0) { if (direction < 0) Stage(row, row->hasMax ? row->max : 100); return true; }
    if (direction < 0 && value <= (row->hasMin ? row->min : 1)) { Stage(row, 4294967295.0); return true; }
    return false;
}

/* The next or the previous option, round the list. */
static void NudgeOption(const Row* row, double value, int direction)
{
    int n = (int)row->options.size(), at = 0;
    for (int i = 0; i < n; i++) if (row->options[i].value == value) at = i;
    Stage(row, row->options[(at + direction + n) % n].value);
}

/* A nudge's step: a tenth of the value's magnitude (whole numbers 1); Shift ten times that, Ctrl a tenth. */
static double NudgeStep(const Row* row, double value, bool shift, bool ctrl)
{
    double step = row->Whole() ? 1 : value == 0 ? 0.1 : pow(10, floor(log10(fabs(value))) - 1);
    if (shift) step *= 10;
    if (ctrl) step = row->Whole() ? 1 : step / 10;
    return step;
}

/* A nudged number as the row takes it: whole, in its range, not negative in a u32. */
static double InRange(const Row* row, double next)
{
    if (row->Whole()) next = llround(next);
    if (row->hasMin) next = std::max(next, row->min);
    if (row->hasMax) next = std::min(next, row->max);
    if (row->type == HF_TYPE_U32) next = std::max(next, 0.0);
    return next;
}

void Nudge(const Row* row, int direction, bool shift, bool ctrl)
{
    if (!row->pick.empty() || !LockOf(row).empty() || !SlotBlock(row).empty()) return;
    RowView v = ViewOf(row);
    if (row->unlimited && NudgeUses(row, v.value, direction)) return;
    if (!row->choice.empty() || row->Valued()) { NudgeOption(row, v.value, direction); return; }
    if (!v.has) return;
    Stage(row, InRange(row, v.value + direction * NudgeStep(row, v.value, shift, ctrl)));
}

/* Reverts a field: its pending change (or pending reset) dropped, else a reset staged. */
void Revert(const Row* row)
{
    if (PendingOf(row)) { for (const Row* s : g_catalog.Siblings(row)) g_pending.erase(s); g_markVersion++; return; }
    StageReset(row);
}
} // namespace editorapp
