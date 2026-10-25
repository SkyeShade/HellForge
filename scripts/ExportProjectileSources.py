"""Export where the round of each weapon HD2Runtime leaves without a projectile swap really comes from, as the editor's
Projectile rows (mappings/projectile-sources.json). Metadata only: weapons, members, record indices, offsets and the stock
projectile type each row starts from (which co-op's shots.c recognises a round by); no other game data.

Usage: py scripts/ExportProjectileSources.py <research/blocked-projectile-swaps> mappings/projectile-sources.json

Reads the research's weapons.json (from a decoded game.dll and HD2Runtime's pinned datalibrary; README.md there has the
evidence). Per weapon, rows of:
  rounds      WeaponRounds +64, a rounds feed with no ammunition delta (AC-8, SG-88, GL-52, EXO-55 right gun, GP-31)
  component   ProjectileWeapon +0 where it is the round after all (the Maelstrom gun, Bastion MG, Maxigun, LAS-17 below
              heat level 1, the Knight's standard round), or +576, a programmable round
  stock       a member with the round's stock type: an ammunition option's entity delta (SG-8, SG-20, Knight, SMG-37;
              WeaponRounds +64 / +68 or ProjectileWeapon +0 as "117:64" / "117:68" / "321:0"), a magazine pattern's entry
              (MG-43, MGX-42, the A/G-16 and A/MG-43 sentries), a charge level's (Purifier, Loyalist, Epoch) or a heat
              level's (LAS-17)
"""
import json
import re
import sys
from pathlib import Path

PREFIX = re.compile(r'^(SHOTGUN 10g\.|PISTOL-SMG \d+x\d+mm\.)\s*', re.I)
RESPAWN = 'Applies to a weapon built after the change (the next call-in or respawn).'


def short(item):
    return PREFIX.sub('', item).strip()


def slug(text):
    return re.sub(r'[^a-z0-9]+', '_', text.lower()).strip('_')


def row(key, label, section, kind, component, resource, record, offset, stock, mode, note=None, shared=()):
    r = {'key': key, 'label': label, 'section': section, 'kind': kind, 'component': component, 'resource': resource, 'record': record,
         'offset': offset, 'stock': stock, 'coop': mode}
    if note:
        r['note'] = note
    if shared:
        r['shared'] = sorted(set(shared))
    return r


def options(w, keyed_default):
    """A weapon's ammunition options as rows, the default first (keyed `ammunition.projectile` when asked)."""
    out, seen = [], {}
    for a in sorted(w['ammunition'], key=lambda a: (not a['default'], a['memberOffset'], a['settingsIndex'])):
        name = short(a['item'])
        n = seen[name] = seen.get(name, 0) + 1
        label_name = name if n == 1 else '%s #%d' % (name, n)
        alt = a['memberOffset'] == 68
        key = 'ammunition.projectile' if keyed_default and a['default'] and not alt else \
            'ammunition.%s%s.projectile' % (slug(label_name), '_alternate' if alt and 'ALT' not in name.upper() else '')
        label = ('Alternate projectile' if alt else 'Projectile') + ' (%s%s)' % (label_name, ', default' if a['default'] else '')
        others = [x for x in set(a['unlockListedBy']) | set(a['defaultOf']) if x != w['weapon']]
        note = RESPAWN + ('' if a['default'] else ' Only while this ammunition is equipped.')
        out.append(row(key, label, 'Ammunition' if a['default'] else 'Ammunition options', 'EntityDelta',
                       '%d:%d' % (a['componentIndex'], a['memberOffset']), a['addPath'], a['settingsIndex'], a['dataOffset'],
                       a['projectile'], 'stock', note, others))
    return out


def export(research):
    data = json.loads((Path(research) / 'weapons.json').read_text(encoding='utf-8'))
    if data['build'] != '25480438' and data.get('gameBuild') != '25480438':
        pass  # the research names the HD2Runtime build id (F5FEE03DCFDB); checked below
    if data['hd2runtimeBuild'] != 'F5FEE03DCFDB':
        raise SystemExit('the research is for another build')
    weapons = {}
    for w in data['weapons']:
        name, res, group = w['weapon'], w['resource'], w['group'].split()[0]
        rec = w['records']
        rows = []
        if w['weapon'] in ('SG-8 Punisher', 'SG-20 Halt'):
            rows = options(w, keyed_default=True)
        elif w['weapon'] == 'SMG-37 Defender':
            rows = options(w, keyed_default=True)
        elif w['weapon'] == 'MP-98 Knight':
            pw = rec['ProjectileWeapon']
            rows = [row('attack.primary.projectile', 'Projectile (standard ammunition)', 'Ammunition', 'component', 'ProjectileWeaponComponentData',
                        res, pw['record'], 0, pw['values']['+0 projectile'], '', 'Its round with no ammunition option equipped; an equipped option replaces it.')]
            rows += options(w, keyed_default=False)
        elif 'patternLayout' in w and w['patternLayout']['magazineType(+0)'] == 1:
            mag = rec['WeaponMagazine']
            entries = w['patternLayout']['entries(+4, u32[32])']
            normal = max(set(entries), key=entries.count)
            for i, t in enumerate(entries):
                tracer = t != normal
                rows.append(row('magazine.pattern_%d.projectile' % (i + 1), 'Projectile (round %d of %d%s)' % (i + 1, len(entries), ', tracer' if tracer else ''),
                                'Magazine pattern', 'component', 'WeaponMagazineComponentData', res, mag['record'], 4 + 4 * i, t, 'stock',
                                'Round n of a magazine fires entry (n - 1) %% %d; the round already chambered keeps its type.' % len(entries)))
        elif 'chargeLevels' in w:
            ch = rec['WeaponCharge']
            levels = w['chargeLevels']
            types = levels['projectiles(+4,+28,+52)']
            labels = ['partial charge', 'full charge', 'overcharge']
            for i, (off, t) in enumerate(zip((4, 28, 52), types)):
                if i == 2 and not levels['overchargeReachable']:
                    continue
                rows.append(row('charge.level_%d.projectile' % (i + 1), 'Projectile (%s)' % labels[i], 'Charge', 'component', 'WeaponChargeComponentData',
                                res, ch['record'], off, t, 'stock', 'Fired on release at this charge (live, from the next release).'))
        elif 'heatLevels' in w:
            pw, heat = rec['ProjectileWeapon'], rec['WeaponHeat']
            rows.append(row('attack.primary.projectile', 'Projectile (below heat level 1)', 'Heat', 'component', 'ProjectileWeaponComponentData', res,
                            pw['record'], 0, pw['values']['+0 projectile'], ''))
            for i, (off, t) in enumerate(zip((4, 28, 52), w['heatLevels']['projectiles(+4,+28,+52)'])):
                rows.append(row('heat.level_%d.projectile' % (i + 1), 'Projectile (heat level %d)' % (i + 1), 'Heat', 'component', 'WeaponHeatComponentData',
                                res, heat['record'], off, t, 'stock', 'Fired at %g heat or more (live, next shot).' % w['heatLevels']['thresholds(+0,+24,+48)'][i]))
        elif group == '2' or 'rounds feed' in w['group']:
            wr = rec['WeaponRounds']
            rows.append(row('attack.feed_primary.projectile', 'Projectile', 'Projectile', 'component', 'WeaponRoundsComponentData', res, wr['record'], 64,
                            wr['values']['+64 primary'], 'r'))
        else:
            pw = rec['ProjectileWeapon']
            rows.append(row('attack.primary.projectile', 'Projectile', 'Projectile', 'component', 'ProjectileWeaponComponentData', res, pw['record'], 0,
                            pw['values']['+0 projectile'], ''))
            if pw['values'].get('+576 functionProjectile'):
                rows.append(row('function_ammo.projectile', 'Programmable ammo projectile', 'Projectile', 'component', 'ProjectileWeaponComponentData',
                                res, pw['record'], 576, pw['values']['+576 functionProjectile'], 'f',
                                'While its weapon function is on, this projectile replaces the fired one.'))
        weapons[name] = {'resource': res, 'path': w['resolverPath'], 'confidence': w['confidence'], 'rows': rows}
    return weapons


def main(research, out):
    weapons = export(research)
    doc = {'format': 1, 'build': '25480438',
           'source': 'HD2Mods research/blocked-projectile-swaps (2026-10-23: decoded game.dll 2E2C3B7C..., HD2Runtime datalibrary, '
                     '10 snapshots); scripts/ExportProjectileSources.py',
           'note': "coop: how shots.c finds the weapon's round: '' ProjectileWeapon +0, 'r' WeaponRounds +64, 'f' +576, 'stock' "
                   "this row's stock type",
           'weapons': dict(sorted(weapons.items()))}
    Path(out).write_text(json.dumps(doc, ensure_ascii=False, indent=1) + '\n', encoding='utf-8', newline='\n')
    print('%d weapons, %d rows' % (len(weapons), sum(len(w['rows']) for w in weapons.values())))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(*sys.argv[1:])
