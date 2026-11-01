/* The editor's window (app_internal.h): the catalogue's lists, an object's page of items, navigation, search, the
 * changes and the errors. */
#include <math.h>
#include <algorithm>
#include "app_internal.h"

namespace editorapp
{
/* ---- the catalogue's lists ---- */
const catalog::Nav* NavOf(int cat) { return cat >= 0 && cat < (int)g_cats.size() && !g_cats[cat].group ? &g_catalog.navs[g_cats[cat].nav] : nullptr; }

std::string Lower(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static std::vector<std::string> Words(const std::string& text)
{
    std::vector<std::string> out;
    std::string word;
    for (char c : Lower(text) + " ")
        if (c == ' ') { if (!word.empty()) out.push_back(word); word.clear(); }
        else word += c;
    return out;
}

std::vector<Object*> Objects()
{
    const catalog::Nav* nav = NavOf(g_cat);
    if (nav == nullptr) return {};
    std::vector<Object*> list = g_catalog.Of(nav->id), out;
    std::string group = g_group.count(nav->id) ? g_group[nav->id] : "All";
    std::string q = Lower(g_search.text);
    for (Object* o : list)
    {
        if (group != "All" && (o->group.empty() ? o->subtitle : o->group) != group) continue;
        if (!q.empty() && (Lower(o->name) + " " + Lower(o->subtitle) + " " + o->search).find(q) == std::string::npos) continue;
        out.push_back(o);
    }
    return out;
}

Object* Current()
{
    auto list = Objects();
    return g_obj >= 0 && g_obj < (int)list.size() ? list[g_obj] : nullptr;
}

static bool IsOpen(const std::string& key) { return g_open.count(key) != 0; }

static std::vector<Item> ItemsNow(const Object* object);
const std::vector<Item>& Items(const Object* object)
{
    static const Object* cachedFor;
    static unsigned frame, version, layout, stats;
    static std::vector<Item> cached;
    unsigned now = localstats::Version();
    if (cachedFor != object || frame != g_frame || version != g_markVersion || layout != g_layoutVersion || stats != now)
    {
        cached = ItemsNow(object);
        cachedFor = object; frame = g_frame; version = g_markVersion; layout = g_layoutVersion; stats = now;
    }
    return cached;
}

/* A part's rows of a weapon's stats: on the weapons page, by the weapon's stats target. */
static std::vector<const Row*> WeaponRows(const catalog::Part& p, const std::string& target)
{
    std::vector<const Row*> rows;
    for (const Row* r : p.rows) if (r->category == "weapons" && r->target == target) rows.push_back(r);
    return rows;
}

/* An object's parts holding a weapon's stats rows, each under its title (a flat category's own). */
static void AddWeaponSections(const Object& o, const std::string& target, std::vector<std::pair<std::string, std::vector<const Row*>>>& sections)
{
    for (auto& c : o.page)
        for (auto& p : c.parts)
            if (std::vector<const Row*> rows = WeaponRows(p, target); !rows.empty()) sections.push_back({c.flat ? c.title : p.title, rows});
}

/* A weapon's stats as its own page shows them (a mount's: "Weapon · Left gun" on its vehicle): each part's title and rows,
 * by the weapon's stats target. Found once. */
static const std::vector<std::pair<std::string, std::vector<const Row*>>>& WeaponSections(const std::string& target)
{
    static std::map<std::string, std::vector<std::pair<std::string, std::vector<const Row*>>>> found;
    auto known = found.find(target);
    if (known != found.end()) return known->second;
    auto& sections = found[target];
    for (auto& o : g_catalog.objects)
    {
        AddWeaponSections(*o, target, sections);
        if (!sections.empty()) break;
    }
    return sections;
}

static void AddNote(std::vector<Item>& items, const std::string& text, int depth) { Item it; it.note = text; it.depth = depth; items.push_back(it); }

/* a row a reference locks is hidden, unless it holds a change of its own (then it shows, LOCKED) */
static bool Hidden(const Row* r) { double held; return !LockOf(r).empty() && !PendingOf(r) && !Saved(r, &held); }

/* The rows of other pages on a settings record's fields (component, ref, offsets), where the catalogue has them, at a
 * depth under a prefix; how many. */
static int AddViews(std::vector<Item>& items, int depth, const std::string& prefix, const char* component, uint64_t ref, std::initializer_list<uint16_t> offsets)
{
    int shown = 0;
    for (uint16_t offset : offsets)
        if (const Row* s = g_catalog.Setting(component, ref, offset)) { Item v; v.row = s; v.depth = depth; v.prefix = prefix; items.push_back(v); shown++; }
    return shown;
}

/* the status in the slot: its duration, for every use of it */
static void AddStatusViews(std::vector<Item>& items, const Row* r, int depth)
{
    RowView v = ViewOf(r);
    if (v.has && v.value != 0) AddViews(items, depth, RefLabel(r, v.value) + " (all uses)", "StatusEffectSettings", (uint64_t)v.value, {40});
}

/* A donor round: why the weapon's own round's rows are hidden, and the donor's round, where the editor has it (edits land
 * on the donor's own item, for everything that fires it). */
static void AddDonorViews(std::vector<Item>& items, const Row* r, int depth, double value)
{
    AddNote(items, "Fires " + RefLabel(r, value) + " now: its own round's rows are hidden until \"" + r->label + "\" is reset", depth);
    std::string donor = "Donor " + RefLabel(r, value);
    int shown = AddViews(items, depth, donor, "ProjectileSettings", (uint64_t)value, {32, 36, 40, 44, 28, 52, 64, 144, 156});
    double hurt;
    if (ReadSetting("ProjectileSettings", (uint64_t)value, 60, HF_TYPE_U32, &hurt) && hurt != 0)
        shown += AddViews(items, depth, donor, "DamageInfo", (uint64_t)hurt, {4, 8, 12, 16, 20, 24, 28, 32, 36, 44, 52, 60, 68});
    if (shown == 0) AddNote(items, donor + ": no item of the editor shows its round", depth);
}

/* the explosion it sets off now: its rows where the catalogue shows them (edits land there) */
static void AddExplosionViews(std::vector<Item>& items, const Row* r, int depth, double value)
{
    std::string name = RefLabel(r, value);
    int shown = AddViews(items, depth, name, "ExplosionSettings", (uint64_t)value, {16, 20, 24});
    if (uint32_t damage = ExplosionDamage((uint32_t)value)) shown += AddViews(items, depth, name, "DamageInfo", damage, {4, 8, 12, 16, 20, 24, 28, 32, 36});
    if (shown == 0) AddNote(items, name + ": no item of the editor shows its fields", depth);
}

/* A row, unless a reference hides it, and what its pick shows after it: the status in its slot, the donor round it
 * fires, the explosion it sets off. */
static void AddRow(std::vector<Item>& items, const Row* r, int depth, const std::string& prefix)
{
    if (Hidden(r)) return;
    Item it; it.row = r; it.depth = depth; it.prefix = prefix; items.push_back(it);
    double value;
    if (r->pick == "status" && r->Live()) AddStatusViews(items, r, depth);
    if (r->pick == "projectile" && Swapped(r, &value)) AddDonorViews(items, r, depth, value);
    if (r->pick != "explosion" || !Swapped(r, &value) || value == 0) return;
    AddExplosionViews(items, r, depth, value);
}

/* The weapon a vehicle weapon's slot holds when it is not the vehicle's own (the category then shows that weapon's
 * stats), else nullptr. */
static const Row::Option* SwappedWeapon(const catalog::Category& c)
{
    for (auto& p : c.parts)
        for (const Row* r : p.rows)
        {
            if (r->mountSlot.empty()) continue;
            const Row* slot = g_catalog.Entry("mounts", MountVehicle(r), r->mountSlot);
            if (slot == nullptr || slot->options.empty()) continue;
            if (const Row::Option* o = slot->OptionOf(ViewOf(slot).value); o != nullptr && o != &slot->options[0]) return o;
        }
    return nullptr;
}

/* a vehicle weapon's category while its slot holds another weapon: that weapon's stats, editable there */
static void AddSwappedCategory(std::vector<Item>& items, const catalog::Category& c, const Row::Option* swapped)
{
    static const std::vector<std::pair<std::string, std::vector<const Row*>>> none;
    auto& sections = swapped->target.empty() ? none : WeaponSections(swapped->target);
    Item h;
    h.header = h.category = true; h.title = c.title + SEP + swapped->label; h.key = c.key; h.open = IsOpen(c.key);
    for (auto& [title, rows] : sections) { h.count += (int)rows.size(); h.rows.insert(h.rows.end(), rows.begin(), rows.end()); }
    h.parts = (int)sections.size();
    items.push_back(h);
    if (!h.open) return;
    if (sections.empty()) { AddNote(items, swapped->label + ": no page of the editor shows its stats", 1); return; }
    for (auto& [title, rows] : sections)
    {
        Item s;
        s.header = true; s.title = title; s.key = c.key + "|" + swapped->target + "|" + title; s.open = IsOpen(s.key);
        s.count = (int)rows.size(); s.depth = 1;
        s.rows = rows;
        items.push_back(s);
        if (s.open) for (const Row* r : rows) AddRow(items, r, 2, "");
    }
}

/* A part of an open category: a folded part is its one row; a part whose rows are all hidden is left out. */
static void AddPart(std::vector<Item>& items, const catalog::Part& p)
{
    if (p.fold) { AddRow(items, p.rows[0], 1, p.prefix); return; }
    if (std::all_of(p.rows.begin(), p.rows.end(), Hidden)) return;
    Item s;
    s.header = true; s.title = p.title; s.key = p.key; s.open = IsOpen(p.key); s.count = (int)p.rows.size(); s.depth = 1;
    s.rows.assign(p.rows.begin(), p.rows.end());
    items.push_back(s);
    if (s.open) for (const Row* r : p.rows) AddRow(items, r, 2, "");
}

static void AddCategory(std::vector<Item>& items, const catalog::Category& c)
{
    Item h;
    h.header = h.category = true; h.title = c.title; h.key = c.key; h.open = IsOpen(c.key); h.count = c.Count(); h.parts = (int)c.parts.size();
    for (auto& p : c.parts) h.rows.insert(h.rows.end(), p.rows.begin(), p.rows.end());
    items.push_back(h);
    if (!h.open) return;
    if (c.flat) { for (const Row* r : c.parts[0].rows) AddRow(items, r, 1, ""); return; }
    for (auto& p : c.parts) AddPart(items, p);
}

static std::vector<Item> ItemsNow(const Object* object)
{
    std::vector<Item> items;
    if (object == nullptr) return items;
    if (!object->detail.empty())
    {
        Item it;
        it.note = object->detail;
        it.detail = true;
        items.push_back(it);
    }
    if (!object->notOffered.empty())
    {
        Item it;
        it.note = "No projectile swap: " + object->notOffered;
        items.push_back(it);
    }
    for (auto& c : object->page)
    {
        if (c.plain) { AddRow(items, c.parts[0].rows[0], 0, c.prefix); continue; }
        if (const Row::Option* swapped = SwappedWeapon(c)) AddSwappedCategory(items, c, swapped);
        else AddCategory(items, c);
    }
    return items;
}

void Toggle(const std::string& key, int open)
{
    bool now = IsOpen(key), want = open < 0 ? !now : open == 1;
    if (want == now) return;
    if (want) g_open.insert(key); else g_open.erase(key);
    g_layoutVersion++;
    SaveSettings();
}

static void SetOpen(const std::string& key, bool open) { if (open) g_open.insert(key); else g_open.erase(key); }

/* A category's header and its parts' headers (a flat category has none for its parts, a folded part none). */
static void SetCategoryOpen(const catalog::Category& c, bool open)
{
    SetOpen(c.key, open);
    if (c.flat) return;
    for (auto& p : c.parts) if (!p.fold) SetOpen(p.key, open);
}

/* Where the focused item is once the headers turned: the same row or header, else the category holding the row; 0 when
 * it is gone. */
static int FocusAfter(const std::vector<Item>& after, const Item& focused)
{
    int field = 0;
    for (int i = 0; i < (int)after.size(); i++)
    {
        if ((focused.row && after[i].row == focused.row) || (!focused.key.empty() && after[i].key == focused.key)) return i;
        if (focused.row && after[i].header && after[i].depth == 0 &&
            std::find(after[i].rows.begin(), after[i].rows.end(), focused.row) != after[i].rows.end()) field = i;
    }
    return field;
}

/* Collapse or expand every header of the page; the focused row stays focused when it is still shown. */
void SetPageOpen(const Object* object, bool open)
{
    if (object == nullptr) return;
    auto before = Items(object);
    g_layoutVersion++;
    Item focused = g_field < (int)before.size() ? before[g_field] : Item{};
    for (auto& c : object->page) if (!c.plain) SetCategoryOpen(c, open);
    SaveSettings();
    auto after = Items(object);
    g_field = FocusAfter(after, focused);
}

bool PageOpen(const Object* object)
{
    if (object == nullptr) return false;
    for (auto& c : object->page)
    {
        if (c.plain) continue;
        if (IsOpen(c.key)) return true;
    }
    return false;
}

std::string RowLabel(const Item& item, float max)
{
    if (item.prefix.empty()) return g_cv.Fit(item.row->label, max, SZ_LABEL);
    std::string rest = SEP + item.row->label;
    float room = max - g_cv.Measure(rest, SZ_LABEL);
    if (g_cv.Measure(item.prefix, SZ_LABEL) <= room) return item.prefix + rest;
    room = std::max(max * 0.3f, room);
    std::string cut = g_cv.Fit(item.prefix, room, SZ_LABEL) + SEP;
    return cut + g_cv.Fit(item.row->label, max - g_cv.Measure(cut, SZ_LABEL), SZ_LABEL);
}

/* ---- the errors list, the confirmation, the logs (HD2R Editor's errors popup, its RESET ITEM confirmation, LOGS) ---- */
ErrorsPopup g_errorsPopup;

/* Every refused field, by object and field. */
std::vector<const Row*> ErrorRows()
{
    std::vector<std::pair<std::string, const Row*>> keyed;
    for (auto& e : g_errors) keyed.emplace_back(Lower(e.first->object->name + " " + e.first->label), e.first);
    std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<const Row*> out;
    for (auto& k : keyed) out.push_back(k.second);
    return out;
}

void OpenErrors() { g_errorsPopup.on = true; g_errorsPopup.index = 0; g_seen.erase("errors"); Sound(sounds::OPEN); }

/* RESET ITEM: the rows of an object holding a change (pending, applied or refused), one per bytes. */
std::vector<const Row*> ItemChanges(const Object* object)
{
    std::vector<const Row*> rows;
    std::set<std::string> seen;
    if (object == nullptr) return rows;
    for (const Row* row : object->rows)
    {
        double v;
        if (seen.count(row->link) || !(PendingOf(row) || Saved(row, &v) || g_errors.count(row))) continue;
        seen.insert(row->link);
        rows.push_back(row);
    }
    return rows;
}

/* The other objects on these rows' bytes, as RESET ITEM's toast names them: three, then how many more. */
static std::string SharedNames(const Object* object, const std::vector<const Row*>& rows)
{
    std::set<std::string> shared;
    for (const Row* r : rows)
        for (const Row* s : g_catalog.Siblings(r)) if (s->object != object) shared.insert(s->object->name);
    std::string names;
    int n = 0;
    for (auto& s : shared) { if (n < 3) names += (n ? ", " : "") + s; n++; }
    if (n > 3) names += " (+" + std::to_string(n - 3) + ")";
    return names;
}

void ResetItem(const Object* object)
{
    if (RefuseLocked()) return;
    auto rows = ItemChanges(object);
    if (rows.empty()) return;
    std::string names = SharedNames(object, rows);
    /* staged only (APPLY writes it, DISCARD takes it back): no confirmation needed */
    int marked = 0, dropped = 0;
    for (const Row* r : rows)
    {
        bool pending = PendingOf(r) != nullptr;
        for (const Row* s : g_catalog.Siblings(r)) g_pending.erase(s);
        if (StageReset(r)) marked++; else if (pending) dropped++;
    }
    g_markVersion++;
    Toast(marked ? std::to_string(marked) + " reset" + (marked == 1 ? "" : "s") + " of " + object->name + " pending: APPLY writes them, DISCARD takes them back" +
            (names.empty() ? "" : " (shared, so also " + names + ")") :
        std::to_string(dropped) + " pending change" + (dropped == 1 ? "" : "s") + " dropped", marked ? PENDING : FAINT);
}

/* ---- navigation ---- */
static std::map<int, std::string> g_selected; /* each category's selected object (its id): back on the category, it is selected again */

void SelectCategory(int i)
{
    if (i < 0 || i >= (int)g_cats.size() || g_cats[i].group) return;
    if (g_cat == i) return;
    auto list = Objects();
    if (g_obj >= 0 && g_obj < (int)list.size()) g_selected[g_cat] = list[g_obj]->id;
    g_cat = i; g_obj = 0; g_field = 0; g_scroll["obj"] = 0; g_scroll["fields"] = 0; g_search = {};
    if (auto kept = g_selected.find(i); kept != g_selected.end())
    {
        list = Objects();
        for (int k = 0; k < (int)list.size(); k++) if (list[k]->id == kept->second) { g_obj = k; g_seen.erase("obj"); break; }
    }
}

void MoveCategory(int delta)
{
    int i = g_cat;
    do i += delta; while (i >= 0 && i < (int)g_cats.size() && g_cats[i].group);
    if (i >= 0 && i < (int)g_cats.size()) SelectCategory(i);
}

void SelectObject(int i)
{
    if (i != g_obj) { g_obj = i; g_field = 0; g_scroll["fields"] = 0; g_seen.erase("fields"); }
}

/* Opens an object in BROWSE: its category (filters cleared) with the object selected. */
bool GotoObject(const Object* object)
{
    for (int i = 0; i < (int)g_cats.size(); i++)
        if (!g_cats[i].group && g_catalog.navs[g_cats[i].nav].id == object->category) SelectCategory(i);
    g_search = {};
    g_group.erase(object->category);
    g_seen.erase("obj");
    auto list = Objects();
    for (int i = 0; i < (int)list.size(); i++) if (list[i] == object) { SelectObject(i); return true; }
    return false;
}

/* Opens a field in BROWSE: its object, the headers holding it opened, the row selected and scrolled into view. */
void Reveal(const Row* row)
{
    GotoObject(row->object);
    for (auto& c : row->object->page)
        for (auto& p : c.parts)
            if (std::find(p.rows.begin(), p.rows.end(), row) != p.rows.end()) { Toggle(c.key, 1); Toggle(p.key, 1); }
    auto items = Items(row->object);
    for (int i = 0; i < (int)items.size(); i++) if (items[i].row == row) { g_field = i; g_scroll["fields"] = std::max(0, i - 5); g_seen.erase("fields"); }
    g_view = BROWSE;
    g_focus = FIELDS;
}

/* ---- search (SEARCH: every object, by name and group; a word of four letters or more also names a category) ---- */
static bool NamesCategory(const std::string& word, const std::string& label)
{
    if (word.size() < 4) return false;
    for (auto& part : Words(label)) if (part.rfind(word, 0) == 0) return true;
    return false;
}

/* Every word is in the object's names (hay), or names its category or group. */
static bool AllWords(const std::vector<std::string>& words, const std::string& hay, const catalog::Nav& nav)
{
    for (auto& w : words)
        if (hay.find(w) == std::string::npos && !NamesCategory(w, nav.label) && !NamesCategory(w, nav.group)) return false;
    return true;
}

std::vector<Result> SearchResults(const std::string& text)
{
    std::vector<Result> out;
    auto words = Words(text);
    if (words.empty()) return out;
    for (auto& nav : g_catalog.navs)
        for (Object* o : g_catalog.Of(nav.id))
        {
            std::string hay = Lower(o->name) + " " + Lower(o->subtitle) + " " + o->search;
            if (AllWords(words, hay, nav))
                out.push_back({o, nav.label + (o->subtitle.empty() ? "" : std::string("  \xC2\xB7  ") + o->subtitle),
                    Lower(o->name).rfind(words[0], 0) == 0, (int)out.size()});
        }
    std::stable_sort(out.begin(), out.end(), [](const Result& a, const Result& b) {
        if (a.prefix != b.prefix) return a.prefix;
        return Lower(a.object->name) < Lower(b.object->name);
    });
    return out;
}

/* ---- CHANGES ---- */

const std::vector<Change>& Changes()
{
    static unsigned stats, version, errors;
    static std::vector<Change> out;
    unsigned now = localstats::Version();
    if (stats == now && version == g_markVersion && errors == g_errorsVersion) return out;
    stats = now; version = g_markVersion; errors = g_errorsVersion;
    out.clear();
    std::set<const Row*> seen;
    for (auto& p : g_pending)
    {
        /* a reset's value (the base) is read when the row is drawn */
        out.push_back({p.first, p.second.value, 1, p.second.reset});
        seen.insert(p.first);
    }
    for (const Row* row : SavedRows())
    {
        if (seen.count(row)) continue;
        double v = 0;
        Saved(row, &v);
        out.push_back({row, v, g_errors.count(row) ? 2 : 0, false});
        seen.insert(row);
    }
    for (auto& e : g_errors) /* a refused change Local Stats does not hold */
        if (!seen.count(e.first)) { out.push_back({e.first, 0, 2, false}); seen.insert(e.first); }
    std::vector<std::pair<std::string, Change>> keyed;
    keyed.reserve(out.size());
    for (auto& c : out) keyed.emplace_back(Lower(c.row->object->name + " " + c.row->label), c);
    std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < keyed.size(); i++) out[i] = keyed[i].second;
    return out;
}
} // namespace editorapp
