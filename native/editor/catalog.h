/* The editor's catalogue (editor-catalog.json, written by HellForge's EditorCatalog.cs next to the DLL): its categories,
 * their objects (a weapon, a throwable) and each object's page of fields, laid out as the HD2R Editor lays out a page. */
#pragma once
#include <stdint.h>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace catalog
{
struct Object;

struct Row
{
    Object* object = nullptr;
    std::string label, category, target, key; /* the Local Stats entry: category, target, key */
    std::string component;                    /* set_live / get_live: the component record or settings table ("" none) */
    uint64_t ref = 0;                         /* the target: an entity's resource id or a table row id */
    uint32_t offset = 0;                      /* an entity delta's is its data offset (past 65535) */
    int difficulty = 0;                       /* a roster row's weight at that difficulty: its icon beside the label (0 none) */
    uint32_t size = 0;                        /* a whole-row copy's bytes (choice "rowcopy") */
    uint32_t type = 0;                        /* HF_TYPE_F32, _U32 or _I32 */
    bool hasMin = false, hasMax = false;
    double min = 0, max = 0;
    std::string unit, applies, link;          /* applies: now, respawn, mission, boot */
    std::vector<std::string> also;            /* the other items a change reaches */
    std::vector<std::string> alsoIf;          /* ... only when they equip the same ammunition (alsoAmmo) */
    std::string alsoAmmo;
    /* A choice row (the armor perks): its choice ("armor", "second"), its note, and its options (value -1: the default). */
    /* target: a mount option's weapon (its stats target), whose rows show under the slot while it is swapped in */
    struct Option { double value; std::string label, icon, note, group; std::vector<std::string> warnings; uint64_t resource = 0; std::string target; };
    std::string choice, note;
    std::vector<Option> options;
    const Option* OptionOf(double value) const;
    /* A reference row (HD2R Editor's payload swaps): what it picks (projectile, explosion, status: the catalogue's
     * references), a status slot's number (its strength row's too) and slot group, the projectile swap row whose donor
     * locks it (its key in the same target), and its default where get_live cannot read one (an ammunition delta). */
    std::string pick, fires, slots;
    std::string mountSlot;                    /* a vehicle weapon's row: the mount slot holding its weapon ("mount.slot_1") */
    std::vector<double> exclude;              /* options this field does not offer (HD2R Editor's exclusions) */
    int slot = 0;
    bool hasBase = false;
    double base = 0;
    bool unlimited = false;                   /* 4294967295 means unlimited (mission uses); -1 in Local Stats */
    /* HD2R Editor's preset keys on its bytes (its own first) and how a preset stores the value: number, uses,
     * status, projectile, explosion, passive */
    std::vector<std::string> hd2r;
    std::string hd2rKind;
    /* keys whose preset value is not the field's number: "bool" (true / false), "enum" (a number or a name), "slots" (a
     * list, this field its slot-th entry), "ap" (an armor penetration label: this trait slot), with the value names */
    struct Hd2rAs { std::string kind; int slot = 0; std::map<std::string, double> names; };
    std::map<std::string, Hd2rAs> hd2rAs;
    /* get_stat: a field get_live cannot read, by the stats snapshot's address (its kind, component, resource, record) */
    std::string statKind, statComponent;
    uint64_t statResource = 0;
    int64_t statRecord = 0;
    bool Stat() const { return !statKind.empty(); }
    bool verified = false, unverified = false, shared = false;
    bool Live() const { return !component.empty(); }
    /* a field with named values only (a flag's No / Yes, an enum, a trait or fire mode slot): picked from its options */
    bool Valued() const { return choice.empty() && !options.empty(); }
    bool Whole() const;
};

struct Part { std::string title, key, prefix; bool fold = false; std::vector<Row*> rows; };
struct Category { std::string title, key, prefix; bool flat = false, plain = false; std::vector<Part> parts; int Count() const; };

struct Object
{
    std::string id, name, category, subtitle, icon, search, tone; /* tone: offensive, defensive, support or none */
    std::string faction;                      /* an entity's: terminids, automatons, illuminate, super_earth or none */
    std::string group;                        /* the filter chip it is under, where the line under its name (subtitle) is not it */
    std::string notOffered;                   /* why HD2R Editor offers no projectile swap for it, or "" */
    std::string sprite, detail;               /* its icon by another name (an armor perk's passive); what it is */
    std::vector<Category> page;
    std::vector<Row*> rows;
};

struct Nav { std::string id, label, group, tone; std::vector<std::string> filters; bool hasFilters = false; };

/* Where a copy of a page points (Catalog::AddCopy): the page's ids for its kinds and the copy's own for each. */
struct Remap { std::map<uint64_t, uint64_t> stratagem, entity, projectile, damage; };

struct Catalog
{
    std::string build;
    std::vector<Nav> navs;
    std::vector<std::unique_ptr<Object>> objects;
    std::deque<Row> rows;
    std::map<std::string, std::vector<Row*>> byLink;      /* rows on the same bytes */
    std::map<std::string, Row*> byEntry;                   /* by Local Stats entry: category \t target \t key */
    std::map<std::string, std::vector<Row::Option>> references; /* what pick rows offer: projectile, explosion, status */
    std::map<std::string, std::string> warnings;           /* a payload option's warnings: id -> what it means */
    /* HD2R Editor presets: rows by key, and the ids a preset stores for references (and back) */
    std::multimap<std::string, Row*> byHd2r;          /* a slot list's key: on each slot's row */
    std::map<std::string, double> hd2rOutputs, hd2rExplosions, hd2rStatuses;
    std::map<double, std::string> outputOf, explosionOf, statusOf;
    std::string hd2rEditor, hd2rRuntime;
    std::map<std::string, Row*> bySetting;                 /* a settings row's field: component \t ref \t offset (the first) */
    const Row::Option* Reference(const std::string& pick, double value) const;
    Row* Setting(const std::string& component, uint64_t ref, uint16_t offset) const;
    std::vector<Object*> Of(const std::string& nav) const;
    /* The object whose page holds the stratagem's settings row (StratagemDefinition of that type), or nullptr. */
    const Object* StratagemObject(uint64_t type) const;
    /* A new object with a copy of like's page pointed through remap (a mod's stratagem: its own settings row, entities,
     * projectile and damage): rows with nothing to point at are left out, and so are rows HellForge's catalogue reads
     * another way (stats snapshot reads, choices, arm swaps). Its rows' Local Stats entry is (category, target, key). */
    Object* AddCopy(const Object& like, const std::string& id, const std::string& name, const std::string& category,
        const std::string& target, const Remap& remap, const std::string& nav = "",
        const std::function<bool(const Row&, uint64_t ref)>& keep = nullptr);
    const std::vector<Row*>& Siblings(const Row* row) const;
    Row* Entry(const std::string& category, const std::string& target, const std::string& key) const;
};

/* Reads the catalogue; false with *error when it is missing or not one this editor reads. */
bool Load(const std::wstring& path, Catalog* out, std::string* error);
} // namespace catalog
