#!/usr/bin/env python3
"""Recompute published summary numbers from the raw measurements.

A review found the write-up quoting one pass in a section that published
another, a ratio computed from the pass that was not published, and figures
with no artefact behind them at all. This exists so that cannot happen again
quietly: it re-derives from raw/**/ and prints what the README should say.

It deliberately does NOT parse the README. Comparing prose to numbers is what
produced the errors in the first place; this prints the numbers, and the
README is expected to match. Run it after any re-measurement:

    python3 benchmarks/05-1c-like-v2/check-summaries.py

Exit status is 1 if an internal consistency check fails, 0 otherwise.
"""
import collections
import csv
import glob
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)
problems = []


def med(xs):
    return statistics.median(xs)


def load_csv(pattern, arm_key, group_key, value_keys):
    """Pull selected columns out of a results-*.csv keyed by arm and group."""
    out = {}
    for path in glob.glob(pattern):
        for row in csv.DictReader(open(path)):
            if row.get('status') not in (None, '', 'OK'):
                continue
            key = (row.get(arm_key, ''), row.get(group_key, ''))
            out[key] = {k: row.get(k, '') for k in value_keys}
    return out


def raw_rows(pattern, ncols, arms):
    """Per-run CSV lines embedded in the pass .txt files."""
    rows = []
    for path in sorted(glob.glob(pattern)):
        for line in open(path):
            f = line.strip().split(',')
            if len(f) == ncols and f[0] in arms:
                rows.append((os.path.basename(path), f))
    return rows


# ---------------------------------------------------------------- growth ----
print('=== Hash Aggregate Growth v1: ladder (from raw/hash-growth/) ===')
rows = raw_rows('raw/hash-growth/2026-09-27-growth-pass*.txt', 33, ('card', 'card2'))
if not rows:
    problems.append('no growth pass files found')
else:
    passes = {p for p, _ in rows}
    print(f'passes={len(passes)}')
    d = collections.defaultdict(list)
    for _, f in rows:
        d[(f[0], f[1], int(f[2]))].append(f)
    hdr = ('tbl', 'arm', 'groups', 'cap', 'load', 'gr', 'agg', 'rehash',
           'agg-re', 'p/lk', 'cur', 'life', 'MB', 'peak', 'n')
    print('  ' + ''.join(h.rjust(w) for h, w in zip(hdr, (7, 12, 8, 8, 8, 4, 7, 8, 8, 8, 5, 6, 7, 7, 4))))
    for (tbl, arm, k), v in sorted(d.items(), key=lambda x: (x[0][0], x[0][1], x[0][2])):
        if v[0][32] == 'ERROR':
            continue
        # counters must be identical within a cell; timings must not be
        for col, name in ((16, 'capacity'), (19, 'load_factor'), (20, 'bytes'),
                          (21, 'bytes_peak'), (26, 'probes_per_lookup'),
                          (27, 'max_probe_current'), (28, 'max_probe_lifetime'),
                          (29, 'growths'), (30, 'rehash_groups')):
            if len({r[col] for r in v}) != 1:
                problems.append(f'{tbl}/{arm}/{k}: {name} varies across runs: '
                                f'{sorted({r[col] for r in v})}')
        u = lambda i: v[0][i]
        cells = (tbl, arm, u(4), u(16), u(19), u(29),
                 f'{med([float(r[10]) for r in v]):.1f}',
                 f'{med([float(r[11]) for r in v]):.2f}',
                 f'{med([float(r[12]) for r in v]):.2f}',
                 u(26), u(27), u(28),
                 f'{int(u(20)) / 1048576:.2f}', f'{int(u(21)) / 1048576:.2f}',
                 str(len(v)))
        print('  ' + ''.join(c.rjust(w) for c, w in zip(cells, (7, 12, 8, 8, 8, 4, 7, 8, 8, 8, 5, 6, 7, 7, 4))))
        # arithmetic that must close
        cap, groups = int(u(16)), int(u(4))
        if int(u(20)) != cap * 32:
            problems.append(f'{tbl}/{arm}/{k}: bytes {u(20)} != capacity*32 {cap * 32}')
        if abs(float(u(19)) - groups / cap) > 5e-4:
            problems.append(f'{tbl}/{arm}/{k}: load_factor {u(19)} != groups/capacity '
                            f'{groups / cap:.4f}')
        g = int(u(29))
        if g:
            init = int(u(15))
            want = sum(init * 2 ** i // 2 for i in range(g))
            if int(u(30)) != want:
                problems.append(f'{tbl}/{arm}/{k}: rehash_groups {u(30)} != doubling '
                                f'series {want} for {g} growths')
            if int(u(21)) != int(u(20)) + int(u(20)) // 2:
                problems.append(f'{tbl}/{arm}/{k}: peak {u(21)} != current+current/2')
        if g == 0 and u(27) != u(28):
            problems.append(f'{tbl}/{arm}/{k}: no growth but probe marks differ '
                            f'{u(27)} vs {u(28)}')

# --------------------------------------------------------- rehash control ----
print('\n=== rehash control (paired, one session) ===')
ctrl = collections.defaultdict(list)
for line in open('raw/hash-growth/2026-09-27-rehash-control.txt'):
    if line.startswith('#') or not line.strip():
        continue
    f = line.strip().split(',')
    ctrl[f[0]].append(f)
if len(ctrl) == 2:
    for arm, v in sorted(ctrl.items()):
        agg = [float(r[5]) for r in v]
        print(f'  {arm:9s} n={len(v)} groups={v[0][2]} cap={v[0][3]} growths={v[0][4]} '
              f'agg med={med(agg):.1f} min={min(agg):.1f} max={max(agg):.1f} '
              f'rehash med={med([float(r[6]) for r in v]):.2f} '
              f'probe cur/life={v[0][8]}/{v[0][9]}')
    nat = [float(r[5]) for r in ctrl['natural']]
    pre = [float(r[5]) for r in ctrl['presized']]
    diff = [a - b for a, b in zip(nat, pre)]
    m = statistics.mean(diff)
    se = statistics.stdev(diff) / len(diff) ** 0.5
    print(f'  paired natural-presized: mean={m:+.2f} ms 95%CI=[{m - 1.96 * se:+.2f},'
          f'{m + 1.96 * se:+.2f}] natural slower in {sum(1 for x in diff if x > 0)}/{len(diff)}')
    print(f'  reported rehash_ms (natural): {med([float(r[6]) for r in ctrl["natural"]]):.2f}')
    if ctrl['natural'][0][3] != ctrl['presized'][0][3]:
        problems.append('rehash control: the two arms did not end at the same capacity')
    if ctrl['natural'][0][8] != ctrl['presized'][0][8]:
        problems.append('rehash control: the two arms did not end with the same '
                        'current probe mark, so the final tables differ')
    # The write-up quotes the pre-sized arm's probe rate as the answering
    # table's figure, so it has to be re-derivable from here.
    pre_plk = med([float(r[7]) for r in ctrl['presized']])
    nat_plk = med([float(r[7]) for r in ctrl['natural']])
    print(f'  probes_per_lookup: pre-sized {pre_plk:.4f} (README quotes 1.205), '
          f'natural {nat_plk:.4f}')
    if abs(pre_plk - 1.205) > 0.001:
        problems.append(f'rehash control: pre-sized probes_per_lookup {pre_plk:.4f} '
                        f'is not the 1.205 the README quotes')
    if not pre_plk < nat_plk:
        problems.append('rehash control: the pre-sized arm should probe less per '
                        'lookup than the arm that grew into the same table')
else:
    problems.append('rehash control file does not hold exactly two arms')

# --------------------------------------------------- earlier benchmarks -----
# Only the figures the write-up quotes in summary tables, recomputed from the
# per-run data, so a later section cannot drift from an earlier one.
print('\n=== 05-C (published pass is runs2) ===')
for tag, path in (('runs1', 'raw/projected/2026-09-24-runs1.csv'),
                  ('runs2', 'raw/projected/2026-09-24-runs2.csv')):
    d = collections.defaultdict(list)
    for line in open(path):
        p = line.strip().split(',')
        if len(p) > 6 and p[0] and p[0] != 'arm':
            try:
                d[(p[0], p[1], p[2])].append(float(p[6]))
            except ValueError:
                pass
    pick = {k: round(med(v), 1) for k, v in d.items()
            if k[0] in ('pgcolumnar', 'fixedlayout-fixed', 'fixedlayout-deform',
                        'fixedlayout-projected', 'bad-projected')}
    print(f'  {tag}: ' + '  '.join(f'{a}[{lo}..{hi}]={x}' for (a, lo, hi), x in sorted(pick.items())))
    if tag == 'runs2':
        f = pick.get(('fixedlayout-fixed', '1', '12'))
        de = pick.get(('fixedlayout-deform', '1', '12'))
        pr = pick.get(('fixedlayout-projected', '1', '12'))
        if None not in (f, de, pr):
            print(f'  runs2 gap closed by projected at 12/12: '
                  f'({de}-{pr})/({de}-{f}) = {(de - pr) / (de - f) * 100:.1f}%')

print('\n=== 05-E (from results-group-cardinality.csv) ===')
e = load_csv('results-group-cardinality.csv', 'arm', 'groups',
             ('aggregate_median_ms', 'source_median_ms', 'load_factor',
              'probes_per_lookup', 'max_probe_lifetime', 'max_probe',
              'agg_ns_per_group'))
for (arm, g), v in sorted(e.items(), key=lambda x: (x[0][0], int(x[0][1] or 0))):
    if arm != 'card-zlfs':
        continue
    print(f'  groups={g:>7s} agg={v["aggregate_median_ms"]:>5s} load={v["load_factor"]:>6s} '
          f'p/lk={v["probes_per_lookup"]:>6s} '
          # this CSV predates the metric split, when the single counter was
          # the lifetime mark under its old name
          f'maxprobe={(v["max_probe_lifetime"] or v["max_probe"]):>3s} '
          f'ns/group={v["agg_ns_per_group"]:>8s}')

print()
if problems:
    print(f'FAIL: {len(problems)} consistency problem(s)')
    for p in problems:
        print(f'  - {p}')
    sys.exit(1)
print('OK: every internal consistency check closes')
