/* The editor's window (app_internal.h): its values. The editor's settings, the co-op role, what a row holds (the game's,
 * a mod's, a saved or a staged value), how it reads, and why a row is locked. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include "app_internal.h"

namespace editorapp
{
std::vector<const Row*> TraitRows(const Row* row)
{
    std::vector<const Row*> out;
    for (const Row* r : row->object->rows)
        if (r->target == row->target && r->key.rfind("presentation.traits.", 0) == 0) out.push_back(r);
    std::sort(out.begin(), out.end(), [](const Row* a, const Row* b) { return a->key < b->key; });
    return out;
}
static double ChoiceGet(const Row* row)
{
    if (!IsMount(row)) return Locked() ? -1 : g_choice[ChoiceIndex(row)];
    auto found = Mounts().find(MountKey(row));
    return found != Mounts().end() ? found->second : row->options.empty() ? -1 : row->options[0].value;
}
bool ChoiceSaved(const Row* row) { return IsMount(row) ? Mounts().count(MountKey(row)) > 0 : !Locked() && g_choice[ChoiceIndex(row)] >= 0; }

/* ---- settings (the editor's folder, editor-settings.json) ---- */
static std::wstring SettingsPath() { return hfjson::Wide(g_host.modFolder) + L"\\editor-settings.json"; }
/* Sandboxed, the game cannot write into its mods folder: settings and exported presets go to the session's sandbox
 * folder, and HellForge puts them in place once the game is gone (SandboxStats.cs). */
static std::wstring SettingsWritePath()
{
    std::wstring sandbox = localstats::SandboxFolder();
    return sandbox.empty() ? SettingsPath() : sandbox + L"\\editor-settings.json";
}

void LoadSettings()
{
    std::string text;
    hfjson::Value root;
    if (!hfjson::ReadFile(SettingsPath(), &text) || !hfjson::Parse(text, &root, nullptr)) return;
    g_soundsOn = root.GetBool("ui_sounds", true);
    g_logSource = (int)root.GetNumber("log_source", 0);
    float scale = (float)root.GetNumber("ui_scale", 1);
    for (float s : UI_SCALES) if (fabsf(s - scale) < 0.001f) g_uiScale = s;
    if (const hfjson::Value* open = root.Find("open"))
        for (const hfjson::Value& v : open->items) if (v.kind == hfjson::Value::String) g_open.insert(v.text);
    if (const hfjson::Value* mounts = root.Find("mounts", hfjson::Value::Object))
        for (auto& m : mounts->members) g_mounts[m.first] = m.second.AsNumber();
    if (const hfjson::Value* passives = root.Find("passives"))
    {
        g_choice[0] = passives->GetNumber("armor", -1);
        g_choice[1] = passives->GetNumber("second", -1);
    }
}

void SaveSettings()
{
    hfjson::Value root;
    root.kind = hfjson::Value::Object;
    root.members.emplace_back("ui_scale", hfjson::Value::MakeNumber(g_uiScale, false));
    root.members.emplace_back("ui_sounds", hfjson::Value::MakeBool(g_soundsOn));
    root.members.emplace_back("log_source", hfjson::Value::MakeNumber(g_logSource, true));
    hfjson::Value open;
    open.kind = hfjson::Value::Array;
    for (auto& key : g_open) open.items.push_back(hfjson::Value::MakeString(key));
    root.members.emplace_back("open", open);
    hfjson::Value passives;
    passives.kind = hfjson::Value::Object;
    passives.members.emplace_back("armor", hfjson::Value::MakeNumber(g_choice[0], true));
    passives.members.emplace_back("second", hfjson::Value::MakeNumber(g_choice[1], true));
    root.members.emplace_back("passives", passives);
    hfjson::Value mounts;
    mounts.kind = hfjson::Value::Object;
    for (auto& m : g_mounts) mounts.members.emplace_back(m.first, hfjson::Value::MakeNumber(m.second, true));
    root.members.emplace_back("mounts", mounts);
    hfjson::WriteAtomically(SettingsWritePath(), hfjson::Write(root));
}

/* ---- values ---- */
std::string Refusal(const char* fallback)
{
    if (g_host.api->size >= offsetof(HfApi, last_log) + sizeof(void*) && g_host.api->last_log != nullptr)
        if (const char* why = g_host.api->last_log(g_host.mod); why != nullptr && why[0]) return why;
    return std::string(fallback) + " (the editor's log, mod-hellforge.editor.log, says why)";
}

bool HasGetLive() { return g_host.api->size >= offsetof(HfApi, get_live) + sizeof(void*) && g_host.api->get_live != nullptr; }
static bool HasGetStat() { return g_host.api->size >= offsetof(HfApi, get_stat) + sizeof(void*) && g_host.api->get_stat != nullptr; }
static bool HasSetStat() { return g_host.api->size >= offsetof(HfApi, set_stat) + sizeof(void*) && g_host.api->set_stat != nullptr; }
/* An attachment's entity delta or a status multiplier: saved like the rest, and (SDK 9, set_stat) changed live too, where
 * the stats pass writes it at boot; a weapon takes it when it is next built (from the next respawn, expected). */
bool LiveStat(const Row* row)
{
    return !row->Live() && row->Stat() && (row->statKind == "EntityDelta" || row->statKind == "StatusMultiplier") && HasSetStat();
}
HfStat StatOf(const Row* row) { return HfStat{row->statKind.c_str(), row->statComponent.c_str(), row->statResource, row->statRecord, row->offset, row->type}; }

/* Co-op (docs/multiplayer.md "Stats in co-op"): every player keeps their own stats, and HellForge loads the packages
 * friends' changes need. A host may share its stats instead (the Mods page): then a joiner plays with the host's, which
 * the session's backend applies, and the editor shows them read-only. A loader from before SDK 15 (no stats_source)
 * always shared the host's. */
HfSession g_session;
bool g_lobbyStats; /* the lobby plays with the host's stats (hosting: this machine's; joined: the host's) */
bool g_hostStats;  /* joined, playing with the host's stats */
void ReadSession()
{
    HfSession now{};
    if (g_host.api->size >= offsetof(HfApi, session) + sizeof(void*) && g_host.api->session != nullptr) g_host.api->session(g_host.mod, &now);
    if (now.version != g_session.version || now.role != g_session.role)
    {
        bool was = g_hostStats, joined = g_session.role == HF_SESSION_JOINER;
        bool sourced = g_host.api->size >= offsetof(HfApi, stats_source) + sizeof(void*) && g_host.api->stats_source != nullptr;
        g_session = now;
        g_lobbyStats = now.role != HF_SESSION_SOLO && (!sourced || g_host.api->stats_source(g_host.mod) == HF_STATS_HOST);
        g_hostStats = now.role == HF_SESSION_JOINER && g_lobbyStats;
        g_markVersion++;
        g_live.clear(); /* the host's values: read again */
        if (!was && g_hostStats) Toast(std::string("Joined ") + now.host + ": you play with the host's stats; the editor is read-only", PENDING);
        else if (was && !g_hostStats) Toast("Back to your own stats: the editor can change them again", OK);
        else if (!joined && now.role == HF_SESSION_JOINER) Toast(std::string("Joined ") + now.host + ": everyone keeps their own changes", OK);
    }
}

/* A change asked for while joined: refused, and said why. */
bool RefuseLocked()
{
    if (!Locked()) return false;
    Toast(std::string("Read-only: you play with ") + (g_session.host[0] ? g_session.host : "the host") + "'s stats while in their lobby", PENDING);
    Sound(sounds::ERROR_);
    return true;
}

HfField FieldOf(const Row* row) { return HfField{row->component.c_str(), g_layout, (uint16_t)row->offset, (uint16_t)(row->type == HF_TYPE_U8 ? 1 : 4), row->type, 0}; }

bool HasPassives() { return g_host.api->size >= offsetof(HfApi, get_passives) + sizeof(void*) && g_host.api->set_passives != nullptr; }

/* The armor perks as the editor keeps them, to the loader. */
void SendPassives()
{
    if (!HasPassives()) return;
    g_host.api->set_passives(g_host.mod, g_choice[0] < 0 ? HF_PASSIVE_KIT : (uint32_t)g_choice[0], g_choice[1] < 0 ? HF_PASSIVE_KIT : (uint32_t)g_choice[1]);
}

/* The armory's armor penetration row: the trait its weapon's trait slots hold (one it names), read through each slot. */
static void LiveApLabel(LiveState& s, const Row* row, bool fresh, bool budgeted)
{
    s.read = true; s.now = s.base = 0; s.mod.clear();
    for (const Row* t : TraitRows(row))
    {
        const LiveState& ts = Live(t, fresh, budgeted);
        if (!ts.read) continue;
        if (ts.now != 0 && row->OptionOf(ts.now)) s.now = ts.now;
        if (ts.base != 0 && row->OptionOf(ts.base)) s.base = ts.base;
        if (!ts.mod.empty()) s.mod = ts.mod;
    }
}

/* What get_live or get_stat answered (read: whether it could), kept from now on. */
static void Took(LiveState& s, bool read, const HfLiveValue& value, ULONGLONG now)
{
    s.read = read;
    if (s.read) { s.now = value.now.number; s.base = value.base.number; s.mod = value.mod ? value.mod : ""; }
    s.at = now;
}

/* One get_live (live) or get_stat read, timed into this frame's live reads. */
static void ReadLive(LiveState& s, const Row* row, bool live, ULONGLONG now)
{
    auto& frame = LiveFrame();
    LONGLONG start = QpcNow();
    HfLiveValue value;
    bool read;
    if (live)
    {
        HfField field = FieldOf(row);
        read = g_host.api->get_live(g_host.mod, row->ref, &field, &value) != 0;
    }
    else
    {
        /* applied at boot: the value the game holds now, and its base (the Local Stats mod's edit undone) */
        HfStat stat = StatOf(row);
        read = g_host.api->get_stat(g_host.mod, &stat, &value) != 0;
    }
    Took(s, read, value, now);
    frame.ticks += QpcNow() - start;
    frame.reads++;
}

LiveState& Live(const Row* row, bool fresh, bool budgeted)
{
    LiveState& s = g_live[row];
    if (IsApLabel(row)) { LiveApLabel(s, row, fresh, budgeted); return s; }
    if (!row->choice.empty())
    {
        s.read = true; s.now = ChoiceGet(row); s.mod.clear();
        s.base = IsMount(row) && !row->options.empty() ? row->options[0].value : -1;
        return s;
    }
    ULONGLONG now = GetTickCount64();
    bool live = row->Live() && HasGetLive(), stat = !row->Live() && row->Stat() && HasGetStat();
    if ((!live && !stat) || !(fresh || s.at == 0 || now - s.at > 400)) return s;
    auto& frame = LiveFrame();
    if (budgeted && !fresh && frame.ticks >= QpcFrequency() * LIVE_BUDGET_US / 1000000) { frame.held++; return s; }
    ReadLive(s, row, live, now);
    return s;
}

bool Same(const Row* row, double a, double b) { return row->Whole() ? llround(a) == llround(b) : (float)a == (float)b; }

/* A reference value's label: the catalogue's name for it, None / Empty for 0, else its number. */
std::string RefLabel(const Row* row, double v)
{
    if (v == 0 && row->pick != "projectile") return row->pick == "status" ? "Empty" : "None";
    if (const Row::Option* o = g_catalog.Reference(row->pick, v)) return o->label;
    char text[48];
    snprintf(text, sizeof text, "%s %.0f", row->pick.c_str(), v);
    return text;
}

/* A number as a field shows it: whole numbers without decimals, tiny ones in %g, the rest with up to four decimals. */
static void FormatNumber(char* text, size_t size, double v, bool whole)
{
    if (whole || (fabs(v) < 1e15 && v == floor(v))) snprintf(text, size, "%.0f", v);
    else if (fabs(v) < 0.001) snprintf(text, size, "%.6g", v);
    else
    {
        snprintf(text, size, "%.4f", v);
        char* end = text + strlen(text) - 1;
        while (end > text && *end == '0') *end-- = 0;
        if (*end == '.') *end = 0;
    }
    if (!strcmp(text, "-0")) strcpy(text, "0");
}

std::string Format(const Row* row, double v)
{
    if (!row->choice.empty()) { const Row::Option* o = row->OptionOf(v); return o ? o->label : "?"; }
    if (row->Valued()) if (const Row::Option* o = row->OptionOf(v)) return o->label; /* else its number */
    if (!row->pick.empty()) return RefLabel(row, v);
    if (row->unlimited && v == 4294967295.0) return "Unlimited";
    char text[64];
    FormatNumber(text, sizeof text, v, row->Whole());
    return text;
}

/* A saved value of the row's bytes (its own entry, else a sibling's on the same bytes). */
bool Saved(const Row* row, double* value, const Row** holder)
{
    if (IsApLabel(row)) return false;
    if (!row->choice.empty()) { *value = ChoiceGet(row); if (holder) *holder = row; return ChoiceSaved(row); }
    auto get = [&](const Row* r) {
        if (!localstats::Get(r->category, r->target, r->key, value)) return false;
        if (r->unlimited && *value == -1) *value = 4294967295.0;
        return true;
    };
    if (get(row)) { if (holder) *holder = row; return true; }
    for (const Row* s : g_catalog.Siblings(row))
        if (s != row && get(s)) { if (holder) *holder = s; return true; }
    return false;
}

const Pend* PendingOf(const Row* row)
{
    auto found = g_pending.find(row);
    if (found != g_pending.end()) return &found->second;
    for (const Row* s : g_catalog.Siblings(row))
    {
        auto other = g_pending.find(s);
        if (other != g_pending.end()) return &other->second;
    }
    return nullptr;
}

static RowView ViewOfNow(const Row* row, bool withPending);
/* A row's view, once a frame (and again after a change): pages, marks and lists ask for it many times. */
RowView ViewOf(const Row* row, bool withPending)
{
    if (!withPending) return ViewOfNow(row, false);
    struct Memo { unsigned frame = 0, version = 0, stats = 0; RowView view; };
    static std::map<const Row*, Memo> memo;
    Memo& m = memo[row];
    unsigned stats = localstats::Version();
    if (m.frame == g_frame && m.version == g_markVersion && m.stats == stats) return m.view;
    m.view = ViewOfNow(row, true);
    m.frame = g_frame; m.version = g_markVersion; m.stats = stats;
    return m.view;
}

/* The armory's armor penetration row: the trait its weapon's trait slots hold (staged when a slot's pick is). */
static RowView ViewOfApLabel(const Row* row, bool withPending)
{
    RowView v;
    bool staged = false;
    v.has = v.hasBase = true;
    for (const Row* t : TraitRows(row))
    {
        RowView tv = ViewOf(t, withPending);
        if (tv.value != 0 && row->OptionOf(tv.value)) v.value = tv.value;
        if (tv.hasBase && tv.base != 0 && row->OptionOf(tv.base)) v.base = tv.base;
        staged = staged || tv.source == STAGED;
    }
    v.source = staged ? STAGED : v.value != v.base ? EDITED : GAME;
    return v;
}

/* A row's view without its pending change: what the game holds (another mod's value), the saved value, or the
 * catalogue's default where get_live cannot read one. */
static RowView HeldView(const Row* row)
{
    RowView v;
    LiveState& live = Live(row);
    double saved;
    /* joined: Saved answers with the host's saved changes, which the game holds (attachments' from the next respawn) */
    bool isSaved = Saved(row, &saved);
    if (live.read)
    {
        v.has = true; v.hasBase = true; v.base = live.base; v.holder = live.mod;
        v.value = !row->Live() && isSaved ? saved : live.now; /* applied at boot: the saved value is the one it will have */
    }
    else if (isSaved) { v.has = true; v.value = saved; }
    if (!live.read && row->hasBase) { v.hasBase = true; v.base = row->base; if (!isSaved) { v.has = true; v.value = row->base; } }
    v.source = isSaved ? EDITED : !v.holder.empty() ? MODDED : GAME;
    return v;
}

/* A pending change (or a pending reset) over the view. */
static void Staged(RowView& v, const Row* row, const Pend* p)
{
    v.source = STAGED;
    v.reset = p->reset || (v.hasBase && Same(row, p->value, v.base));
    if (p->reset) { v.has = v.hasBase; v.value = v.base; }
    else { v.has = true; v.value = p->value; }
}

static RowView ViewOfNow(const Row* row, bool withPending)
{
    if (IsApLabel(row)) return ViewOfApLabel(row, withPending);
    RowView v = HeldView(row);
    if (withPending && !Locked())
        if (const Pend* p = PendingOf(row)) Staged(v, row, p);
    return v;
}

/* The damage type an explosion type deals (ExplosionSettings +4), read through get_live and kept for a while; 0 unknown. */
static std::map<uint32_t, std::pair<uint32_t, ULONGLONG>> g_explosionDamage;
uint32_t ExplosionDamage(uint32_t type)
{
    if (type == 0 || !HasGetLive()) return 0;
    auto& known = g_explosionDamage[type];
    ULONGLONG now = GetTickCount64();
    if (known.second == 0 || now - known.second > 2000)
    {
        HfField field{"ExplosionSettings", g_layout, 4, 4, HF_TYPE_U32, 0};
        HfLiveValue value;
        known.first = g_host.api->get_live(g_host.mod, type, &field, &value) ? (uint32_t)value.now.number : 0;
        known.second = now;
    }
    return known.first;
}

/* One u32 or f32 of a settings row, read through get_live; false when it cannot be read. */
bool ReadSetting(const char* component, uint64_t ref, uint16_t offset, uint32_t type, double* out)
{
    if (!HasGetLive()) return false;
    HfField field{component, g_layout, offset, 4, type, 0};
    HfLiveValue value;
    if (!g_host.api->get_live(g_host.mod, ref, &field, &value)) return false;
    *out = value.now.number;
    return true;
}

/* An explosion as the HD2R Editor's picker sums it up ("225 damage · 4 to 7 m · 1 status"), read from the game and
 * kept for a while; "" when it cannot be read. */
static std::map<uint32_t, std::pair<std::string, ULONGLONG>> g_explosionSummary;
std::string ExplosionSummary(uint32_t type)
{
    auto& known = g_explosionSummary[type];
    ULONGLONG now = GetTickCount64();
    if (known.second != 0 && now - known.second < 5000) return known.first;
    known.second = now;
    double inner, outer, damage = 0;
    if (!ReadSetting("ExplosionSettings", type, 16, HF_TYPE_F32, &inner) || !ReadSetting("ExplosionSettings", type, 20, HF_TYPE_F32, &outer))
        return known.first = "";
    uint32_t hurt = ExplosionDamage(type);
    int statuses = 0;
    if (hurt != 0)
    {
        ReadSetting("DamageInfo", hurt, 4, HF_TYPE_I32, &damage);
        for (uint16_t k = 0; k < 4; k++) { double s; if (ReadSetting("DamageInfo", hurt, (uint16_t)(44 + 8 * k), HF_TYPE_U32, &s) && s != 0) statuses++; }
    }
    char text[96];
    snprintf(text, sizeof text, "%.0f damage \xC2\xB7 %g to %g m%s", damage, floor(inner * 10 + 0.5) / 10, floor(outer * 10 + 0.5) / 10,
        statuses ? (statuses == 1 ? " \xC2\xB7 1 status" : (" \xC2\xB7 " + std::to_string(statuses) + " statuses").c_str()) : "");
    return known.first = text;
}

/* A payload warning's chip (HD2R Editor's badges). */
std::string WarningChip(const std::string& id)
{
    if (id == "very_large") return "VERY LARGE";
    if (id == "lingering_field") return "LINGERING";
    if (id == "no_damage") return "NO DAMAGE";
    if (id == "objective_packages") return "300 MB LOAD";
    std::string upper = id;
    for (char& c : upper) c = c == '_' ? ' ' : (char)toupper((unsigned char)c);
    return upper;
}

/* The vehicle a vehicle page's row belongs to (its Mounted Weapons rows' target). */
std::string MountVehicle(const Row* row)
{
    for (const Row* r : row->object->rows) if (IsMount(r)) return r->target;
    return "";
}

/* A reference row that names something other than its default (a donor projectile, another explosion). */
bool Swapped(const Row* row, double* value)
{
    RowView v = ViewOf(row);
    *value = v.value;
    return v.has && v.hasBase && !Same(row, v.value, v.base);
}

/* Why a row is locked by a reference (HD2R Editor's): the weapon's own round while it fires a donor, the own explosion of
 * a slot (and its damage) while the slot sets off another; "" when it is not. */
static std::string LockOfNow(const Row* row);
std::string LockOf(const Row* row)
{
    struct Memo { unsigned frame = 0, version = 0, stats = 0; std::string why; };
    static std::map<const Row*, Memo> memo;
    Memo& m = memo[row];
    unsigned stats = localstats::Version();
    if (m.frame == g_frame && m.version == g_markVersion && m.stats == stats) return m.why;
    m.why = LockOfNow(row);
    m.frame = g_frame; m.version = g_markVersion; m.stats = stats;
    return m.why;
}

/* a vehicle weapon's rows while its slot holds another weapon (HD2R Editor refuses them then) */
static bool MountLock(const Row* row, std::string* why)
{
    if (row->mountSlot.empty()) return false;
    const Row* slot = g_catalog.Entry("mounts", row->object->rows.empty() ? "" : MountVehicle(row), row->mountSlot);
    if (slot == nullptr || !ChoiceSaved(slot)) return false;
    const Row::Option* o = slot->OptionOf(ChoiceGet(slot));
    if (o == nullptr || o == &slot->options[0]) return false;
    *why = "\"" + slot->label + "\" holds " + o->label + " now: reset it to edit this weapon";
    return true;
}

/* the weapon's own round while it fires a donor */
static bool RoundLock(const Row* row, std::string* why)
{
    if (row->fires.empty()) return false;
    double value;
    const Row* swap = g_catalog.Entry(row->category, row->target, row->fires);
    if (swap == nullptr || !Swapped(swap, &value)) return false;
    *why = "the weapon fires " + RefLabel(swap, value) + " now: reset \"Projectile\" to edit its own round";
    return true;
}

/* the own explosion of a slot (and its damage) while the slot sets off another */
static bool ExplosionLock(const Row* row, std::string* why)
{
    if (row->component != "ExplosionSettings" && row->component != "DamageInfo") return false;
    double value;
    for (const Row* slot : row->object->rows)
    {
        if (slot->pick != "explosion" || slot->target != row->target || !Swapped(slot, &value)) continue;
        uint32_t own = (uint32_t)llround(ViewOf(slot).base);
        if (own == 0) continue;
        if ((row->component == "ExplosionSettings" && row->ref == own) || (row->component == "DamageInfo" && row->ref == ExplosionDamage(own)))
        {
            *why = "\"" + slot->label + "\" sets off " + RefLabel(slot, value) + " now: reset it to edit its own explosion";
            return true;
        }
    }
    return false;
}

static std::string LockOfNow(const Row* row)
{
    std::string why;
    if (MountLock(row, &why) || RoundLock(row, &why)) return why;
    ExplosionLock(row, &why);
    return why;
}

/* The status slots fill in order, from slot 1 (HD2R Editor's slot_block): why a slot (or its strength) waits, or "". */
std::string SlotBlock(const Row* row)
{
    if (row->slot == 0) return "";
    auto type = [&](int k) { return g_catalog.Entry(row->category, row->target, row->slots + "status_" + std::to_string(k) + "_type"); };
    if (row->pick.empty())
    {
        const Row* own = type(row->slot);
        return own != nullptr && ViewOf(own).value == 0 ? "pick \"Status " + std::to_string(row->slot) + "\" first" : "";
    }
    const Row* before = row->slot > 1 ? type(row->slot - 1) : nullptr;
    if (before != nullptr && ViewOf(before).value == 0 && ViewOf(row).value == 0)
        return "pick \"Status " + std::to_string(row->slot - 1) + "\" first: the status slots fill in order, from slot 1";
    return "";
}

/* Whether emptying a status slot would leave a gap (a later slot holds a status). */
bool LaterSlotFilled(const Row* row)
{
    for (int k = row->slot + 1; k <= 4; k++)
        if (const Row* later = g_catalog.Entry(row->category, row->target, row->slots + "status_" + std::to_string(k) + "_type"); later && ViewOf(later).value != 0)
            return true;
    return false;
}

std::string AppliesText(const Row* row)
{
    if (LiveStat(row)) return "applies on respawn (expected)";
    std::string when = row->applies == "now" ? "applies now" : row->applies == "respawn" ? "applies on respawn" :
        row->applies == "mission" ? "applies next mission" : "applies from the next boot";
    return row->Live() && !row->verified ? when + " (expected)" : when;
}
} // namespace editorapp
