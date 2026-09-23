# Historical results

Kept for the record. Not comparable with current numbers: different machine,
and in one case a dataset this repository can no longer build.

## v0.1.0 — experiment 2, two batch hash joins

    path          median_total  source  join1  join2   agg  runs  groups
    zlfs_2joins          6.2 ms  <0.1     1.8    1.5   2.9     5     200
    heap_2joins          282 ms   275     1.9    1.5   3.0     5     200
    pg_vanilla          1194 ms     —       —      —     —     —     200

    checksum b90fc574dab20f03c7a295f8e74fcb83

**Status: historical, currently unreproducible.** The dimension tables that run
joined against were never committed. `dim_period` has since been recovered from
an older `bench/00_setup.sql` in a saved snapshot, but `dim_account` has not:
the snapshot's version has 500 rows, and the v0.1.0 run recorded 200 result
groups, so it is not the table that run used. The checksum depends on the
`account_group` VALUES, so it cannot be reached from this tree and has not been
reverse-engineered to match.

What the v0.1.0 record does pin — 1M input rows and 200 result groups — holds
for the current dataset too.
