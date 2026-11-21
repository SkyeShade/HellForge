"""Put our explosion names (mappings/explosion-names.json) into the files that show them.

Usage: py scripts/ExportExplosionNames.py [mappings folder]

- mappings/references.json: a catalogued explosion's label becomes our name; HD2Runtime's label stays as `was`, a
  stats file's alias for it. `explosionNames` lists the named explosions the catalogue has no row for (the editor's and
  the stats pages' names for them).
- mappings/catalog.json: the SDK's explosion names (HF_EXPLOSION_* and hf->find). A renamed one keeps its constant, so
  mods built against it still compile.

ExportReferences.py runs this after each export. Run it after editing explosion-names.json, then
`hellforge sdk-headers --repo .`.
"""
import json
import sys
from pathlib import Path


def constant(name):
    """SdkMappings.Constant: "MG-206 Heavy Machine Gun" -> MG206_HEAVY_MACHINE_GUN."""
    out = ''
    for ch in name.replace('-', '').replace('/', '').upper():
        if 'A' <= ch <= 'Z' or '0' <= ch <= '9':
            out += ch
        elif out and out[-1] != '_':
            out += '_'
    return out.strip('_')


def load_names(folder):
    path = Path(folder) / 'explosion-names.json'
    if not path.exists():
        return {}
    data = json.loads(path.read_text(encoding='utf-8'))
    if data.get('build') != '25480438':
        raise ValueError('explosion-names.json is for another build')
    names = {int(t): n for t, n in data['names'].items()}
    for t, n in names.items():
        if not 1 <= t <= 422 or n.get('certainty') not in ('sure', 'likely') or not n.get('name') or not n.get('evidence'):
            raise ValueError(f'explosion-names.json: type {t} needs a name, a certainty (sure or likely) and its evidence')
    labels = [n['name'] for n in names.values()]
    if len(set(labels)) != len(labels):
        raise ValueError('explosion-names.json: names must be unique')
    return names


def apply(folder):
    folder = Path(folder)
    names = load_names(folder)

    refs_path = folder / 'references.json'
    refs = json.loads(refs_path.read_text(encoding='utf-8'))
    for r in refs['explosions']:
        original = r.pop('was', None) or r['label']
        n = names.get(r['type'])
        r['label'] = n['name'] if n else original
        if r['label'] != original:
            r['was'] = original
    catalogued = {r['type'] for r in refs['explosions']}

    cat_path = folder / 'catalog.json'
    cat_raw = cat_path.read_text(encoding='utf-8')
    cat = json.loads(cat_raw)
    entries = {int(v['id']): (k, v) for k, v in cat['explosions'].items()}
    final = {r['type']: r['label'] for r in refs['explosions']}
    for t, n in names.items():
        final[t] = n['name']
    out = {}
    for t in sorted(set(entries) | set(final)):
        key, entry = entries.get(t, (None, {'id': str(t)}))
        entry = dict(entry)
        label = final.get(t, key)
        if key is not None and label != key and 'constant' not in entry:
            entry['constant'] = constant(key)  # the SDK's constant stays as it was
        n = names.get(t)
        if n:
            entry['source'] = 'mappings/explosion-names.json'
            if n['certainty'] == 'likely' and 'likely name' not in entry.get('note', ''):
                entry['note'] = entry['note'] + '; likely name' if 'note' in entry else 'likely name (mappings/explosion-names.json says why)'
        out[label] = {k: entry[k] for k in ('constant', 'id', 'note', 'source') if k in entry}
    if len(out) != len(set(final) | set(entries)):
        raise ValueError('explosion names clash with other explosions\' labels')
    cat['explosions'] = out
    cat_path.write_text(json.dumps(cat, indent=2, ensure_ascii=False) + ('\n' if cat_raw.endswith('\n') else ''), encoding='utf-8',
                        newline='\n')

    refs['explosionNames'] = [{'type': t, 'label': label} for t, label in sorted((t, k) for k, v in out.items() for t in [int(v['id'])])
                              if t not in catalogued]
    refs_path.write_text(json.dumps(refs, indent=2, ensure_ascii=False) + '\n', encoding='utf-8', newline='\n')
    sure = sum(1 for n in names.values() if n['certainty'] == 'sure')
    print(f'{len(out)} of 422 explosions named ({len(names)} ours: {sure} sure, {len(names) - sure} likely); '
          f'{len(refs["explosionNames"])} without a catalogue row')


if __name__ == '__main__':
    if len(sys.argv) > 2:
        sys.exit(__doc__)
    apply(sys.argv[1] if len(sys.argv) == 2 else Path(__file__).parent.parent / 'mappings')
