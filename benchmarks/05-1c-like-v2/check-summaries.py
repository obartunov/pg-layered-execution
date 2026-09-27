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
import re
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

# ------------------------------------------------- dimension growth --------
# Slot sizes and initial capacities as declared in xpb_v2_report.c
# (sizeof(V2Dim1)=12, sizeof(V2Dim2)=24, V2_DIM1_CAP=256, V2_DIM2_CAP=1024).
DIM_ELEM = {'dim1': 12, 'dim2': 24}
DIM_INIT_CAP = {'dim1': 256, 'dim2': 1024}

print('\n=== Dimension Hash Growth v1 (from raw/dim-growth/) ===')
drows = raw_rows('raw/dim-growth/2026-09-27-dimgrowth-pass*.txt', 39,
                 {str(n) for n in (64, 128, 129, 192, 384, 768)})
if not drows:
    problems.append('no dimension-growth pass files found')
else:
    print(f'passes={len({p for p, _ in drows})}')
    dd = collections.defaultdict(list)
    # Same rows indexed by pass as well, for the flatness baseline below: it
    # needs the spread of per-pass medians, not of raw runs.
    per_pass = collections.defaultdict(lambda: collections.defaultdict(list))
    for p, f in drows:
        k = (int(f[0]), int(f[1]), int(f[2]))
        dd[k].append(f)
        per_pass[k][p].append(f)
    print(f'  {"comp":>5s}{"acct":>6s}{"groups":>8s}{"d1cap":>7s}{"d1e":>6s}{"d1ld":>8s}'
          f'{"d1g":>4s}{"d2cap":>7s}{"d2e":>6s}{"d2ld":>8s}{"d2g":>4s}'
          f'{"join1":>7s}{"join2":>7s}{"n":>4s}')
    for k in sorted(dd):
        v = dd[k]
        u = lambda i: v[0][i]
        for col, name in ((14, 'dim1_cap'), (15, 'dim1_entries'), (16, 'dim1_load'),
                          (17, 'dim1_growths'), (26, 'dim2_cap'), (27, 'dim2_entries'),
                          (28, 'dim2_load'), (29, 'dim2_growths'), (4, 'groups')):
            if len({r[col] for r in v}) != 1:
                problems.append(f'dim point {k}: {name} varies across runs')
        print(f'  {k[0]:>5d}{k[1]:>6d}{u(4):>8s}{u(14):>7s}{u(15):>6s}{float(u(16)):8.4f}'
              f'{u(17):>4s}{u(26):>7s}{u(27):>6s}{float(u(28)):8.4f}{u(29):>4s}'
              f'{med([float(r[10]) for r in v]):7.1f}{med([float(r[11]) for r in v]):7.1f}'
              f'{len(v):>4d}')
        # the policy invariant, and the arithmetic that must close
        for cap, ent, load, gro, bts, pk, cxt, what in (
                (14, 15, 16, 17, 23, 24, 25, 'dim1'), (26, 27, 28, 29, 35, 36, 37, 'dim2')):
            c, e = int(u(cap)), int(u(ent))
            if e > c // 2:
                problems.append(f'dim point {k}: {what} holds {e} entries in {c} slots, '
                                f'above the 0.5 growth policy')
            if abs(float(u(load)) - e / c) > 5e-4:
                problems.append(f'dim point {k}: {what} load {u(load)} != {e}/{c}')
            # sizeof(V2Dim1) = {bool, int32, int32}       -> 12 on LP64
            # sizeof(V2Dim2) = {bool, int64, int32}       -> 24 on LP64
            # These are the struct layouts in xpb_v2_report.c, not fitted to the
            # measurement: the reported byte count has to equal capacity times
            # the slot size or one of the two is wrong.
            elem = DIM_ELEM[what]
            if int(u(bts)) != c * elem:
                problems.append(f'dim point {k}: {what} bytes {u(bts)} != capacity*{elem}')
            if int(u(gro)) and int(u(pk)) != int(u(bts)) + int(u(bts)) // 2:
                problems.append(f'dim point {k}: {what} peak {u(pk)} != current+current/2')

            # Independent leak evidence, with AllocSet's freelist accounted for.
            #
            # MemoryContextMemAllocated counts malloc'd blocks, not live chunks.
            # An allocation ABOVE aset.c's ALLOC_CHUNK_LIMIT (8192) gets a block
            # of its own and that block is returned to malloc on pfree, so a
            # surviving predecessor of that size would show up here. An
            # allocation AT OR BELOW the limit is served from a shared block and
            # pfree only puts the chunk on a freelist -- the block stays. So the
            # context legitimately exceeds the live table by the blocks holding
            # every sub-limit generation this table passed through.
            #
            # That residue is bounded without reference to the measurement: the
            # generations are the doubling sequence from the initial capacity,
            # and only those at or below 8192 bytes are retained. The factor of
            # two per retained generation is a slack allowance, NOT aset.c's
            # rule -- a sub-limit chunk lands in a block of nextBlockSize, a
            # doubling sequence from initBlockSize that is not a function of the
            # chunk size, so the block holding it can be larger or smaller than
            # twice the chunk. The bound is therefore generous rather than
            # derived, and a violation means a leak large enough to clear a
            # generous bound. Plus one keeper block of initBlockSize
            # (ALLOCSET_DEFAULT_SIZES = 8192, allocated at context creation).
            #
            # dim2 never has a sub-limit generation, so its bound reduces to
            # keeper + live + header and is exact at every point; only dim1's
            # residue term carries the slack.
            # Halving the final capacity once per recorded growth must land back
            # on the declared initial capacity -- that is the "doubled every
            # time" half of the policy, checked against a constant.
            gen = int(u(cap)) >> int(u(gro))
            if gen != DIM_INIT_CAP[what]:
                problems.append(f'dim point {k}: {what} final cap {u(cap)} after '
                                f'{u(gro)} growths implies initial {gen}, not '
                                f'{DIM_INIT_CAP[what]} -- growth was not a pure doubling')
            residue = 8192
            while gen * elem <= 8192:
                residue += 2 * gen * elem
                gen *= 2
            live = int(u(bts)) + (48 if int(u(bts)) > 8192 else 0)
            if int(u(cxt)) > live + residue:
                problems.append(f'dim point {k}: {what} context holds {u(cxt)} for a '
                                f'{u(bts)} byte table, above {live + residue} '
                                f'(live + retained sub-chunk-limit generations) '
                                f'-- a predecessor may have survived')

    # --- the timings the README section quotes, and the flatness claim -------
    print(f'  {"comp":>5s}{"total":>8s}{"source":>8s}{"d1build":>9s}{"d2build":>9s}'
          f'{"join1":>7s}{"join2":>7s}{"agg":>7s}{"d1reh":>8s}{"d2reh":>8s}')
    j1s, j2s, tots = [], [], []
    for k in sorted(dd):
        v = dd[k]
        q = lambda i: med([float(r[i]) for r in v])
        j1s.append(q(10)); j2s.append(q(11)); tots.append(q(6))
        print(f'  {k[0]:>5d}{q(6):8.1f}{q(7):8.1f}{q(8):9.3f}{q(9):9.3f}'
              f'{q(10):7.1f}{q(11):7.1f}{q(12):7.1f}{q(19):8.3f}{q(31):8.3f}')
    # "join probe cost flat across a 12x change" needs a baseline, and the only
    # honest one is this dataset's own noise: if the variation ACROSS
    # cardinalities is no larger than the variation WITHIN a single
    # cardinality, the ladder has not resolved an effect. No external
    # threshold, so nothing here can be tuned to the answer.
    # Both sides must be the SAME statistic or the comparison is not like for
    # like: an earlier version put the across-ladder spread of 25-run medians
    # against the raw min-max inside one cell, which is an extreme value
    # dominated by a single spike and left 15x headroom -- a 60% regression
    # would have passed. Both sides are now spreads of per-pass medians.
    for name, col, xs in (('join1', 10, j1s), ('join2', 11, j2s)):
        across = (max(xs) - min(xs)) / med(xs)
        within = 0.0
        for k in dd:
            pm = [med([float(r[col]) for r in rs])
                  for rs in per_pass[k].values() if rs]
            if len(pm) > 1:
                within = max(within, (max(pm) - min(pm)) / med(pm))
        print(f'  {name}: across-ladder spread of medians {across * 100:.1f}%, '
              f'widest within-cardinality spread of per-pass medians '
              f'{within * 100:.1f}%')
        if across >= within:
            problems.append(f'dim ladder: {name} varies more across the ladder '
                            f'({across * 100:.1f}%) than within a single cardinality '
                            f'({within * 100:.0f}%) -- an effect may be resolved and '
                            f'"no effect resolved" is not supported')
    # "4x the old fixed limit": the old limit was 3/4 of 256 and of 1024.
    top = dd[max(dd)]
    for ent, old_limit, what in ((15, 256 * 3 // 4, 'dim1'), (27, 1024 * 3 // 4, 'dim2')):
        got = int(top[0][ent])
        if got != 4 * old_limit:
            problems.append(f'dim ladder: {what} top point holds {got} entries, not 4x the '
                            f'old {old_limit}-key limit -- the README\'s "4x" is wrong')

    # ----------------------------- paired pre-sized control -----------------
    print('\n=== dimension pre-sized control (paired, one connection) ===')
    cpairs = collections.defaultdict(dict)
    for path in sorted(glob.glob('raw/dim-growth/2026-09-27-dimgrowth-pass*.txt')):
        after = False
        for line in open(path):
            if 'paired pre-sized control' in line:
                after = True
                continue
            if not after:
                continue
            f = line.strip().split(',')
            if len(f) == 13 and f[0] in ('natural', 'presized'):
                cpairs[(path, f[1])][f[0]] = f
    full = [v for v in cpairs.values() if len(v) == 2]
    if not full:
        problems.append('no dimension pre-sized control pairs found')
    else:
        bld = lambda r: float(r[6]) + float(r[7])
        reh = lambda r: float(r[8]) + float(r[9])
        nat_b = med([bld(v['natural']) for v in full])
        pre_b = med([bld(v['presized']) for v in full])
        nat_r = med([reh(v['natural']) for v in full])
        extra = [bld(v['natural']) - bld(v['presized']) for v in full]
        unacc = [bld(v['natural']) - bld(v['presized']) - reh(v['natural']) for v in full]
        tot = [float(v['natural'][10]) - float(v['presized'][10]) for v in full]
        print(f'  pairs={len(full)}')
        print(f'  build d1+d2: natural={nat_b:.3f} presized={pre_b:.3f} '
              f'delta={nat_b - pre_b:+.3f} ms')
        print(f'  reported rehash (natural): {nat_r:.3f} ms')
        print(f'  per-pair extra build: med={med(extra):+.3f} '
              f'[{min(extra):+.3f}..{max(extra):+.3f}]')
        print(f'  per-pair unaccounted:  med={med(unacc):+.3f} '
              f'[{min(unacc):+.3f}..{max(unacc):+.3f}]')
        print(f'  per-pair total delta:  med={med(tot):+.2f} mean={statistics.mean(tot):+.2f} '
              f'[{min(tot):+.1f}..{max(tot):+.1f}] natural slower in '
              f'{sum(1 for x in tot if x > 0)}/{len(tot)}')
        # the control is only paired if both arms really ended identically
        for fld, name in ((2, 'dim1 capacity'), (3, 'dim2 capacity')):
            if len({v[a][fld] for v in full for a in ('natural', 'presized')}) != 1:
                problems.append(f'dim control: the arms did not all end at the same {name}')
        if not all(int(v['presized'][4]) == 0 and int(v['presized'][5]) == 0 for v in full):
            problems.append('dim control: the pre-sized arm grew, so it is not a control')
        if not all(int(v['natural'][4]) > 0 and int(v['natural'][5]) > 0 for v in full):
            problems.append('dim control: the natural arm did not grow both tables')
        # The README states the rehash counter fully explains the extra build
        # cost, and states it ONLY at build granularity because the total
        # cannot resolve it. Both halves have to hold.
        if abs(med(unacc)) > 0.05:
            problems.append(f'dim control: {med(unacc):+.3f} ms of the extra build time is '
                            f'not explained by the reported rehash -- the README claims '
                            f'the counter accounts for it')
        if not (min(unacc) < 0 < max(unacc)):
            problems.append('dim control: the unaccounted residual does not straddle zero, '
                            'so it is a real effect and not noise as the README says')
        if abs(med(tot)) > 2.0:
            problems.append(f'dim control: paired total delta median {med(tot):+.2f} ms is '
                            f'large enough to quote -- the README says it cannot be resolved')

    # The README says both arms produce the same result. Capacities and growth
    # counts do not show that: the two arms reach the same capacity by different
    # insert orders, so their slot contents differ. Only a per-group comparison
    # rules out one arm having lost an entry, and it lives in its own artifact
    # so that it does not perturb the five published measurement passes.
    arm = glob.glob('raw/dim-growth/*-dimgrowth-armcheck.txt')
    if not arm:
        problems.append('no dim arm-equivalence artifact found, but the README claims '
                        'both control arms produce the same result')
    else:
        txt = open(arm[0]).read()
        cks = set(re.findall(r'^\s+(?:presized|natural)\s+([0-9a-f]{32})\s', txt, re.M))
        if 'arm equivalence PASS' not in txt or len(cks) != 1:
            problems.append(f'dim arm equivalence not established: {len(cks)} distinct '
                            f'checksum(s), gate line '
                            f'{"present" if "arm equivalence PASS" in txt else "absent"}')
        else:
            print(f'  arm equivalence: both arms -> {cks.pop()[:12]}... (one checksum)')

# ------------------------------------- committed CSV must not drift from raw --
# results-dim-growth.csv is published alongside the raw passes. Nothing else
# reads it, which is exactly how a hand-assembled summary drifts, so its every
# cell is checked against the medians re-derived from raw/ above.
if drows:
    csvp = 'results-dim-growth.csv'
    with open(csvp) as fh:
        crows = {(int(r['n_comp']), int(r['n_acct'])): r for r in csv.DictReader(fh)}
    if set(crows) != {(k[0], k[1]) for k in dd}:
        problems.append(f'{csvp}: point set differs from raw/')
    else:
        COLS = {'groups': 4, 'rows': 5, 'total_median_ms': 6, 'source_median_ms': 7,
                'dim1_build_median_ms': 8, 'dim2_build_median_ms': 9,
                'join1_median_ms': 10, 'join2_median_ms': 11,
                'aggregate_median_ms': 12, 'operators_median_ms': 13,
                'dim1_capacity': 14, 'dim1_entries': 15, 'dim1_load': 16,
                'dim1_growths': 17, 'dim1_rehash_entries': 18,
                'dim1_rehash_median_ms': 19, 'dim2_capacity': 26,
                'dim2_entries': 27, 'dim2_load': 28, 'dim2_growths': 29,
                'dim2_rehash_entries': 30, 'dim2_rehash_median_ms': 31}
        bad = 0
        for k in sorted(dd):
            v, r = dd[k], crows[(k[0], k[1])]
            for name, col in COLS.items():
                want = med([float(x[col]) for x in v])
                got = float(r[name])
                if abs(got - want) > 5e-4 + abs(want) * 1e-9:
                    problems.append(f'{csvp} {k[0]}/{k[1]}: {name}={got} but raw/ gives {want}')
                    bad += 1
        if not bad:
            print(f'  {csvp}: all {len(crows) * len(COLS)} published cells match raw/')

print()
if problems:
    print(f'FAIL: {len(problems)} consistency problem(s)')
    for p in problems:
        print(f'  - {p}')
    sys.exit(1)
print('OK: every internal consistency check closes')
