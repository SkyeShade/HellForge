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

Object* Catalog::AddCopy(const Object& like, const std::string& id, const std::string& name, const std::string& category,
    const std::string& target, const Remap& remap, const std::string& nav, const std::function<bool(const Row&, uint64_t ref)>& keep)
{
    auto object = std::make_unique<Object>();
    Object* copy = object.get();
    copy->id = id;
    copy->name = name;
    copy->category = nav.empty() ? like.category : nav;
    copy->subtitle = like.subtitle;
    copy->icon = like.icon;
    copy->tone = like.tone;
    for (const Nav& n : navs) if (n.id == copy->category) copy->tone = n.tone;
    copy->sprite = like.sprite.empty() ? like.name : like.sprite; /* its icon: the page's, unless it has one of its own */
    copy->search = name + " " + like.subtitle + " " + like.category;
    for (char& c : copy->search) c = (char)tolower((unsigned char)c);
    auto point = [&](const Row& r, uint64_t* ref) {
        const std::map<uint64_t, uint64_t>* map = r.component == "StratagemDefinition" ? &remap.stratagem
            : r.component == "ProjectileSettings" ? &remap.projectile : r.component == "DamageInfo" ? &remap.damage
            : r.component.find("ComponentData") != std::string::npos ? &remap.entity : nullptr;
        if (map == nullptr) return false;
        auto found = map->find(r.ref);
        if (found == map->end()) return false;
        *ref = found->second;
        return true;
    };
    for (const Category& c : like.page)
    {
        Category category2{c.title, id + c.key.substr(c.key.find('|')), c.prefix, c.flat, c.plain, {}};
        for (const Part& p : c.parts)
        {
            Part part{p.title, id + p.key.substr(p.key.find('|')), p.prefix, p.fold, {}};
            for (const Row* r : p.rows)
            {
                uint64_t ref;
                if (!r->Live() || r->Stat() || !r->choice.empty() || !r->mountSlot.empty() || !point(*r, &ref) || (keep && !keep(*r, ref))) continue;
                rows.push_back(*r);
                Row& row = rows.back();
                row.object = copy;
                row.ref = ref;
                row.category = category;
                row.target = target;
                row.link = "mod|" + r->component + ":" + std::to_string(ref) + ":" + std::to_string(r->offset);
                row.also.clear();
                row.alsoIf.clear();
                row.alsoAmmo.clear();
                row.hd2r.clear();
                row.hd2rAs.clear();
                if (r->component.find("ComponentData") != std::string::npos) row.shared = false; /* the copy's own record */
                part.rows.push_back(&row);
                copy->rows.push_back(&row);
                byLink[row.link].push_back(&row);
                byEntry[row.category + '\t' + row.target + '\t' + row.key] = &row;
            }
            if (!part.rows.empty()) category2.parts.push_back(part);
        }
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

bool Load(const std::wstring& path, Catalog* out, std::string* error)
{
    std::string text;
    if (!hfjson::ReadFile(path, &text)) { *error = "editor-catalog.json is missing (rebuild the editor)"; return false; }
    Value root;
    if (!hfjson::Parse(text, &root, error)) return false;
    if (root.GetNumber("format") != 1) { *error = "editor-catalog.json is of another format (rebuild the editor)"; return false; }
    out->build = root.GetString("build");
    if (const Value* navs = root.Find("categories"))
        for (const Value& n : navs->items)
        {
            Nav nav{n.GetString("id"), n.GetString("label"), n.GetString("group"), n.GetString("tone"), {}, false};
            if (const Value* filters = n.Find("filters"); filters != nullptr && filters->kind == Value::Array)
            {
                nav.hasFilters = true;
                for (const Value& f : filters->items) nav.filters.push_back(f.Str());
            }
            out->navs.push_back(nav);
        }
    if (const Value* objects = root.Find("objects"))
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
            if (const Value* page = o.Find("page"))
                for (const Value& c : page->items)
                {
                    /* headers open and close per item (its id first): one weapon's open category stays its own */
                    Category category{c.GetString("title"), object->id + "|" + c.GetString("key"), c.GetString("prefix"), c.GetBool("flat"), c.GetBool("plain"), {}};
                    if (const Value* parts = c.Find("subs"))
                        for (const Value& s : parts->items)
                        {
                            Part part{s.GetString("title"), object->id + "|" + s.GetString("key"), s.GetString("prefix"), s.GetBool("fold"), {}};
                            if (const Value* rows = s.Find("rows"))
                                for (const Value& r : rows->items)
                                {
                                    out->rows.emplace_back();
                                    Row& row = out->rows.back();
                                    row.object = object.get();
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
                                    const Value* min = r.Find("min");
                                    const Value* max = r.Find("max");
                                    row.hasMin = min != nullptr && min->kind == Value::Number;
                                    row.hasMax = max != nullptr && max->kind == Value::Number;
                                    if (row.hasMin) row.min = min->AsNumber();
                                    if (row.hasMax) row.max = max->AsNumber();
                                    row.unit = r.GetString("unit");
                                    row.applies = r.GetString("applies", "boot");
                                    row.verified = r.GetBool("verified");
                                    row.unverified = r.GetBool("unverified");
                                    row.shared = r.GetBool("shared");
                                    row.link = r.GetString("link");
                                    if (const Value* also = r.Find("also"); also != nullptr && also->kind == Value::Array)
                                        for (const Value& a : also->items) row.also.push_back(a.Str());
                                    if (const Value* alsoIf = r.Find("alsoIf"); alsoIf != nullptr && alsoIf->kind == Value::Array)
                                        for (const Value& a : alsoIf->items) row.alsoIf.push_back(a.Str());
                                    row.alsoAmmo = r.GetString("alsoAmmo");
                                    row.choice = r.GetString("choice");
                                    row.size = (uint32_t)r.GetNumber("size", 0);
                                    row.note = r.GetString("note");
                                    if (const Value* options = r.Find("options"); options != nullptr && options->kind == Value::Array)
                                        for (const Value& o : options->items)
                                        {
                                            row.options.push_back({o.GetNumber("value"), o.GetString("label"), o.GetString("icon"), o.GetString("note")});
                                            row.options.back().target = o.GetString("target");
                                            row.options.back().group = o.GetString("group");
                                            std::string resource = o.GetString("resource");
                                            if (resource.rfind("0x", 0) == 0) row.options.back().resource = strtoull(resource.c_str() + 2, nullptr, 16);
                                        }
                                    /* a field with named values (a flag, an enum, a trait or fire mode slot) */
                                    if (const Value* values = r.Find("values"); values != nullptr && values->kind == Value::Array && row.choice.empty())
                                        for (const Value& o : values->items) row.options.push_back({o.GetNumber("value"), o.GetString("label")});
                                    row.pick = r.GetString("pick");
                                    row.fires = r.GetString("fires");
                                    row.mountSlot = r.GetString("mountSlot");
                                    row.slots = r.GetString("slots");
                                    row.slot = (int)r.GetNumber("slot", 0);
                                    row.unlimited = r.GetBool("unlimited");
                                    if (const Value* keys = r.Find("hd2r"); keys != nullptr && keys->kind == Value::Array)
                                        for (const Value& k : keys->items) row.hd2r.push_back(k.Str());
                                    row.hd2rKind = r.GetString("hd2rKind", "number");
                                    if (const Value* as = r.Find("hd2rAs"); as != nullptr && as->kind == Value::Object)
                                        for (auto& [key, a] : as->members)
                                        {
                                            Row::Hd2rAs& h = row.hd2rAs[key];
                                            h.kind = a.GetString("kind");
                                            h.slot = (int)a.GetNumber("slot");
                                            if (const Value* names = a.Find("names"); names != nullptr && names->kind == Value::Object)
                                                for (auto& [name, n] : names->members) h.names[name] = n.AsNumber();
                                        }
                                    if (const Value* exclude = r.Find("exclude"); exclude != nullptr && exclude->kind == Value::Array)
                                        for (const Value& e : exclude->items) row.exclude.push_back(e.AsNumber());
                                    if (const Value* stat = r.Find("stat"); stat != nullptr && stat->kind == Value::Array && stat->items.size() == 4)
                                    {
                                        row.statKind = stat->items[0].Str();
                                        row.statComponent = stat->items[1].Str();
                                        std::string resource = stat->items[2].Str();
                                        row.statResource = resource.rfind("0x", 0) == 0 ? strtoull(resource.c_str() + 2, nullptr, 16) : 0;
                                        row.statRecord = (int64_t)stat->items[3].AsNumber();
                                    }
                                    if (const Value* base = r.Find("base"); base != nullptr && base->kind == Value::Number) { row.hasBase = true; row.base = base->AsNumber(); }
                                    if (row.type == 0 || (!row.choice.empty() && row.options.empty())) { out->rows.pop_back(); continue; }
                                    part.rows.push_back(&row);
                                    object->rows.push_back(&row);
                                    out->byLink[row.link].push_back(&row);
                                    out->byEntry[row.category + '\t' + row.target + '\t' + row.key] = &row;
                                    for (auto& k : row.hd2r) out->byHd2r.emplace(k, &row);
                                    if (row.Live() && row.component.find("ComponentData") == std::string::npos)
                                        out->bySetting.emplace(SettingKey(row.component, row.ref, row.offset), &row);
                                }
                            if (!part.rows.empty()) category.parts.push_back(part);
                        }
                    if (!category.parts.empty()) object->page.push_back(category);
                }
            out->objects.push_back(std::move(object));
        }
    if (const Value* references = root.Find("references"); references != nullptr && references->kind == Value::Object)
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
                if (const Value* warnings = o.Find("warnings"); warnings != nullptr && warnings->kind == Value::Array)
                    for (const Value& w : warnings->items) option.warnings.push_back(w.Str());
                out->references[list.first].push_back(option);
            }
        }
    if (const Value* hd2r = root.Find("hd2r"); hd2r != nullptr && hd2r->kind == Value::Object)
    {
        auto map = [&](const char* name, std::map<std::string, double>* ids, std::map<double, std::string>* back) {
            if (const Value* m = hd2r->Find(name); m != nullptr && m->kind == Value::Object)
                for (auto& e : m->members)
                {
                    double type = e.second.AsNumber();
                    (*ids)[e.first] = type;
                    back->emplace(type, e.first); /* the first id (in key order) names a type on export */
                }
        };
        map("outputs", &out->hd2rOutputs, &out->outputOf);
        map("explosions", &out->hd2rExplosions, &out->explosionOf);
        map("statuses", &out->hd2rStatuses, &out->statusOf);
        out->hd2rEditor = hd2r->GetString("editor");
        out->hd2rRuntime = hd2r->GetString("runtime");
    }
    if (out->objects.empty()) { *error = "editor-catalog.json lists nothing"; return false; }
    return true;
}
} // namespace catalog
