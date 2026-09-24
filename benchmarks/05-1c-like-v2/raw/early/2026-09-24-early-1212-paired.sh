#!/bin/bash
# 12/12 control only: alternate late/early within one session, 15 pairs each
# layout.  Block-ordered runs let slow drift land entirely on one arm; strict
# alternation makes every pair a same-conditions comparison.
set -euo pipefail
PSQL=(psql -p 5420 -h /tmp/xpb_sock -d onec2 -v ON_ERROR_STOP=1 -X -qAt)
GUC="SET work_mem='64MB'; SET enable_hashjoin=on; SET jit=off; SET max_parallel_workers_per_gather=0;"
one() {
  "${PSQL[@]}" -c "$GUC SELECT count(*) FROM xpb_v2_register_report(1,12,'$1')" 2>&1 \
    | sed -n 's/^NOTICE:  v2_register_report //p' | sed -n 's/.*source=\([0-9.]*\) ms.*/\1/p'
}
echo "# layout,pair,late_ms,early_ms"
for lay in bad fixedlayout; do
  one "$lay-projected" >/dev/null; one "$lay-early" >/dev/null   # warm-up pair
  for i in $(seq 1 15); do
    l=$(one "$lay-projected"); e=$(one "$lay-early")
    echo "$lay,$i,$l,$e"
  done
done
