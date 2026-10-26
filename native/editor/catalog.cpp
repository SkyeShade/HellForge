/* The editor's catalogue (catalog.h). */
#include <ctype.h>
#include <stdlib.h>
#include "catalog.h"
#include "hellforge_mod.h"
#include "json.h"

using hfjson::Value;

namespace catalog
{
bool Row::Whole() const { return type != HF_TYPE_F32; }

const Row::Option* Row::OptionOf(double value) const
{
    for (auto& o : options) if (o.value == value) return &o;
    return nullptr;
}

int Category::Count() const
{
    int n = 0;
    for (auto& p : parts) n += (int)p.rows.size();
    return n;
}

std::vector<Object*> Catalog::Of(const std::string& nav) const
{
    std::vector<Object*> out;
    for (auto& o : objects) if (o->category == nav) out.push_back(o.get());
    return out;
}

const std::vector<Row*>& Catalog::Siblings(const Row* row) const
{
    static const std::vector<Row*> none;
    auto found = byLink.find(row->link);
    return found != byLink.end() ? found->second : none;
}

const Row::Option* Catalog::Reference(const std::string& pick, double value) const
{
    auto list = references.find(pick);
    if (list == references.end()) return nullptr;
    for (auto& o : list->second) if (o.value == value) return &o;
    return nullptr;
}

static std::string SettingKey(const std::string& component, uint64_t ref, uint16_t offset)
{
    return component + '\t' + std::to_string(ref) + '\t' + std::to_string(offset);
}

Row* Catalog::Setting(const std::string& component, uint64_t ref, uint16_t offset) const
{
    auto found = bySetting.find(SettingKey(component, ref, offset));
    return found != bySetting.end() ? found->second : nullptr;
}

Row* Catalog::Entry(const std::string& category, const std::string& target, const std::string& key) const
{
    auto found = byEntry.find(category + '\t' + target + '\t' + key);
    return found != byEntry.end() ? found->second : nullptr;
}

const Object* Catalog::StratagemObject(uint64_t type) const
{
    for (auto& o : objects)
        for (const Row* r : o->rows)
            if (r->component == "StratagemDefinition" && r->ref == type && o->id.rfind("mod/", 0) != 0) return o.get();
    return nullptr;
}

/* Where a row of like's page points in the copy (its kind's entry in remap); false when it has nothing to point at. */
static bool Pointed(const Row& r, const Remap& remap, uint64_t* ref)
{
    const std::map<uint64_t, uint64_t>* map = r.component == "StratagemDefinition" ? &remap.stratagem
        : r.component == "ProjectileSettings" ? &remap.projectile : r.component == "DamageInfo" ? &remap.damage
        : r.component.find("ComponentData") != std::string::npos ? &remap.entity : nullptr;
    if (map == nullptr) return false;
    auto found = map->find(r.ref);
    if (found == map->end()) return false;
    *ref = found->second;
    return true;
}

/* A part of like's page in the copy: its rows pointed through remap (into's own, on its indexes), the rows with nothing
 * to point at and those HellForge's catalogue reads another way left out. */
static Part CopyPart(Catalog* into, const Part& p, Object* copy, const CopyNames& names, const Remap& remap,
    const std::function<bool(const Row&, uint64_t ref)>& keep)
{
    Part part{p.title, names.id + p.key.substr(p.key.find('|')), p.prefix, p.fold, {}};
    for (const Row* r : p.rows)
    {
        uint64_t ref;
        if (!r->Live() || r->Stat() || !r->choice.empty() || !r->mountSlot.empty() || !Pointed(*r, remap, &ref) || (keep && !keep(*r, ref))) continue;
        into->rows.push_back(*r);
        Row& row = into->rows.back();
        row.object = copy;
        row.ref = ref;
        row.category = names.category;
        row.target = names.target;
        row.link = "mod|" + r->component + ":" + std::to_string(ref) + ":" + std::to_string(r->offset);
        row.also.clear();
        row.alsoIf.clear();
        row.alsoAmmo.clear();
        row.hd2r.clear();
        row.hd2rAs.clear();
        if (r->component.find("ComponentData") != std::string::npos) row.shared = false; /* the copy's own record */
        part.rows.push_back(&row);
        copy->rows.push_back(&row);
        into->byLink[row.link].push_back(&row);
        into->byEntry[row.category + '\t' + row.target + '\t' + row.key] = &row;
    }
    return part;
}

Object* Catalog::AddCopy(const Object& like, const CopyNames& names, const Remap& remap, const std::function<bool(const Row&, uint64_t ref)>& keep)
{
    auto object = std::make_unique<Object>();
    Object* copy = object.get();
    copy->id = names.id;
    copy->name = names.name;
    copy->category = names.nav.empty() ? like.category : names.nav;
    copy->subtitle = like.subtitle;
    copy->icon = like.icon;
    copy->tone = like.tone;
    for (const Nav& n : navs) if (n.id == copy->category) copy->tone = n.tone;
    copy->sprite = like.sprite.empty() ? like.name : like.sprite; /* its icon: the page's, unless it has one of its own */
    copy->search = names.name + " " + like.subtitle + " " + like.category;
    for (char& c : copy->search) c = (char)tolower((unsigned char)c);
    for (const Category& c : like.page)
    {
        Category category2{c.title, names.id + c.key.substr(c.key.find('|')), c.prefix, c.flat, c.plain, {}};
        for (const Part& p : c.parts)
            if (Part part = CopyPart(this, p, copy, names, remap, keep); !part.rows.empty()) category2.parts.push_back(part);
        if (!category2.parts.empty()) copy->page.push_back(category2);
    }
    if (copy->rows.empty()) return nullptr;
    objects.push_back(std::move(object));
    return copy;
}

static uint32_t TypeOf(const std::string& storage)
{
    return storage == "f32" ? HF_TYPE_F32 : storage == "u32" ? HF_TYPE_U32 : storage == "i32" ? HF_TYPE_I32 : storage == "u64" ? HF_TYPE_U64 :
        storage == "u8" ? HF_TYPE_U8 : 0;
}

/* A list of strings into out; false when there is none (no member of that name, or not a list). */
static bool Strings(const Value& v, const char* name, std::vector<std::string>* out)
{
    const Value* list = v.Find(name, Value::Array);
    if (list == nullptr) return false;
    for (const Value& s : list->items) out->push_back(s.Str());
    return true;
}

/* A resource id written "0x..." (else 0). */
static uint64_t Hex(const std::string& text) { return text.rfind("0x", 0) == 0 ? strtoull(text.c_str() + 2, nullptr, 16) : 0; }

static void LoadNavs(const Value& root, Catalog* out)
{
    const Value* navs = root.Find("categories");
    if (navs == nullptr) return;
    for (const Value& n : navs->items)
    {
        Nav nav{n.GetString("id"), n.GetString("label"), n.GetString("group"), n.GetString("tone"), {}, false};
        nav.hasFilters = Strings(n, "filters", &nav.filters);
        out->navs.push_back(nav);
    }
}

static void ReadRange(const Value& r, Row& row)
{
    const Value* min = r.Find("min", Value::Number);
    const Value* max = r.Find("max", Value::Number);
    row.hasMin = min != nullptr;
    row.hasMax = max != nullptr;
    if (row.hasMin) row.min = min->AsNumber();
    if (row.hasMax) row.max = max->AsNumber();
}

static void ReadOptions(const Value& r, Row& row)
{
    if (const Value* options = r.Find("options", Value::Array))
        for (const Value& o : options->items)
        {
            row.options.push_back({o.GetNumber("value"), o.GetString("label"), o.GetString("icon"), o.GetString("note")});
            row.options.back().target = o.GetString("target");
            row.options.back().group = o.GetString("group");
            row.options.back().resource = Hex(o.GetString("resource"));
        }
    /* a field with named values (a flag, an enum, a trait or fire mode slot) */
    if (const Value* values = r.Find("values", Value::Array); values != nullptr && row.choice.empty())
        for (const Value& o : values->items) row.options.push_back({o.GetNumber("value"), o.GetString("label")});
}

static void ReadHd2rAs(const Value& r, Row& row)
{
    const Value* as = r.Find("hd2rAs", Value::Object);
    if (as == nullptr) return;
    for (auto& [key, a] : as->members)
    {
        Row::Hd2rAs& h = row.hd2rAs[key];
        h.kind = a.GetString("kind");
        h.slot = (int)a.GetNumber("slot");
        if (const Value* names = a.Find("names", Value::Object))
            for (auto& [name, n] : names->members) h.names[name] = n.AsNumber();
    }
}

static void ReadStat(const Value& r, Row& row)
{
    const Value* stat = r.Find("stat", Value::Array);
    if (stat == nullptr || stat->items.size() != 4) return;
    row.statKind = stat->items[0].Str();
    row.statComponent = stat->items[1].Str();
    row.statResource = Hex(stat->items[2].Str());
    row.statRecord = (int64_t)stat->items[3].AsNumber();
}

/* A row's fields, as the catalogue gives them. */
static void ReadRow(const Value& r, Row& row)
{
    row.label = r.GetString("label");
    row.difficulty = (int)r.GetNumber("difficulty");
    row.category = r.GetString("cat");
    row.target = r.GetString("target");
    row.key = r.GetString("key");
    row.component = r.GetString("component");
    std::string ref = r.GetString("ref");
    row.ref = ref.rfind("0x", 0) == 0 ? strtoull(ref.c_str() + 2, nullptr, 16) : strtoull(ref.c_str(), nullptr, 10);
    row.offset = (uint32_t)r.GetNumber("offset");
    row.type = TypeOf(r.GetString("type"));
    ReadRange(r, row);
    row.unit = r.GetString("unit");
    row.applies = r.GetString("applies", "boot");
    row.verified = r.GetBool("verified");
    row.unverified = r.GetBool("unverified");
    row.shared = r.GetBool("shared");
    row.link = r.GetString("link");
    Strings(r, "also", &row.also);
    Strings(r, "alsoIf", &row.alsoIf);
    row.alsoAmmo = r.GetString("alsoAmmo");
    row.choice = r.GetString("choice");
    row.size = (uint32_t)r.GetNumber("size", 0);
    row.note = r.GetString("note");
    ReadOptions(r, row);
    row.pick = r.GetString("pick");
    row.fires = r.GetString("fires");
    row.mountSlot = r.GetString("mountSlot");
    row.slots = r.GetString("slots");
    row.slot = (int)r.GetNumber("slot", 0);
    row.unlimited = r.GetBool("unlimited");
    Strings(r, "hd2r", &row.hd2r);
    row.hd2rKind = r.GetString("hd2rKind", "number");
    ReadHd2rAs(r, row);
    if (const Value* exclude = r.Find("exclude", Value::Array))
        for (const Value& e : exclude->items) row.exclude.push_back(e.AsNumber());
    ReadStat(r, row);
    if (const Value* base = r.Find("base", Value::Number)) { row.hasBase = true; row.base = base->AsNumber(); }
}

/* A row of the part, unless it is one the editor cannot show (a storage it does not know, a choice with no options). */
static void LoadRow(const Value& r, Object* object, Part* part, Catalog* out)
{
    out->rows.emplace_back();
    Row& row = out->rows.back();
    row.object = object;
    ReadRow(r, row);
    if (row.type == 0 || (!row.choice.empty() && row.options.empty())) { out->rows.pop_back(); return; }
    part->rows.push_back(&row);
    object->rows.push_back(&row);
    out->byLink[row.link].push_back(&row);
    out->byEntry[row.category + '\t' + row.target + '\t' + row.key] = &row;
    for (auto& k : row.hd2r) out->byHd2r.emplace(k, &row);
    if (row.Live() && row.component.find("ComponentData") == std::string::npos)
        out->bySetting.emplace(SettingKey(row.component, row.ref, row.offset), &row);
}

/* A part of a page's category and its rows (a part left with no row is left out). */
static void LoadPart(const Value& s, Object* object, Category* category, Catalog* out)
{
    Part part{s.GetString("title"), object->id + "|" + s.GetString("key"), s.GetString("prefix"), s.GetBool("fold"), {}};
    if (const Value* rows = s.Find("rows"))
        for (const Value& r : rows->items) LoadRow(r, object, &part, out);
    if (!part.rows.empty()) category->parts.push_back(part);
}

/* An object's page: its categories, their parts and rows (a category left with no part is left out). */
static void LoadPage(const Value& o, Object* object, Catalog* out)
{
    const Value* page = o.Find("page");
    if (page == nullptr) return;
    for (const Value& c : page->items)
    {
        /* headers open and close per item (its id first): one weapon's open category stays its own */
        Category category{c.GetString("title"), object->id + "|" + c.GetString("key"), c.GetString("prefix"), c.GetBool("flat"), c.GetBool("plain"), {}};
        if (const Value* parts = c.Find("subs"))
            for (const Value& s : parts->items) LoadPart(s, object, &category, out);
        if (!category.parts.empty()) object->page.push_back(category);
    }
}

static void LoadObjects(const Value& root, Catalog* out)
{
    const Value* objects = root.Find("objects");
    if (objects == nullptr) return;
    for (const Value& o : objects->items)
    {
        auto object = std::make_unique<Object>();
        object->id = o.GetString("id");
        object->name = o.GetString("name");
        object->category = o.GetString("category");
        object->subtitle = o.GetString("subtitle");
        object->icon = o.GetString("icon");
        object->search = o.GetString("search");
        object->tone = o.GetString("tone");
        object->faction = o.GetString("faction");
        object->group = o.GetString("group");
        object->notOffered = o.GetString("notOffered");
        object->sprite = o.GetString("sprite");
        object->detail = o.GetString("detail");
        LoadPage(o, object.get(), out);
        out->objects.push_back(std::move(object));
    }
}

static void LoadReferences(const Value& root, Catalog* out)
{
    const Value* references = root.Find("references", Value::Object);
    if (references == nullptr) return;
    for (auto& list : references->members)
    {
        if (list.first == "warnings")
        {
            for (auto& w : list.second.members) out->warnings[w.first] = w.second.Str();
            continue;
        }
        for (const Value& o : list.second.items)
        {
            Row::Option option{o.GetNumber("value"), o.GetString("label"), "", o.GetString("note"), o.GetString("group"), {}};
            Strings(o, "warnings", &option.warnings);
            out->references[list.first].push_back(option);
        }
    }
}

/* HD2R Editor's ids of one kind of reference (a preset stores them), and back. */
static void LoadIds(const Value& hd2r, const char* name, std::map<std::string, double>* ids, std::map<double, std::string>* back)
{
    const Value* m = hd2r.Find(name, Value::Object);
    if (m == nullptr) return;
    for (auto& e : m->members)
    {
        double type = e.second.AsNumber();
        (*ids)[e.first] = type;
        back->emplace(type, e.first); /* the first id (in key order) names a type on export */
    }
}

static void LoadHd2r(const Value& root, Catalog* out)
{
    const Value* hd2r = root.Find("hd2r", Value::Object);
    if (hd2r == nullptr) return;
    LoadIds(*hd2r, "outputs", &out->hd2rOutputs, &out->outputOf);
    LoadIds(*hd2r, "explosions", &out->hd2rExplosions, &out->explosionOf);
    LoadIds(*hd2r, "statuses", &out->hd2rStatuses, &out->statusOf);
    out->hd2rEditor = hd2r->GetString("editor");
    out->hd2rRuntime = hd2r->GetString("runtime");
}

bool Load(const std::wstring& path, Catalog* out, std::string* error)
{
    std::string text;
    if (!hfjson::ReadFile(path, &text)) { *error = "editor-catalog.json is missing (rebuild the editor)"; return false; }
    Value root;
    if (!hfjson::Parse(text, &root, error)) return false;
    if (root.GetNumber("format") != 1) { *error = "editor-catalog.json is of another format (rebuild the editor)"; return false; }
    out->build = root.GetString("build");
    LoadNavs(root, out);
    LoadObjects(root, out);
    LoadReferences(root, out);
    LoadHd2r(root, out);
    if (out->objects.empty()) { *error = "editor-catalog.json lists nothing"; return false; }
    return true;
}
} // namespace catalog
