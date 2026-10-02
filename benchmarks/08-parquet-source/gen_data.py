#!/usr/bin/env python3
"""
Deterministic Parquet dataset for the XPBatch Parquet source experiment.

  benchmarks/08-parquet-source/gen_data.py <outdir>

Writes orders.parquet plus orders.csv and schema.sql, so PostgreSQL can hold
byte-identical logical data and act as the oracle.

No randomness anywhere. Every value is a closed-form function of the row index,
so the file is reproducible and any disagreement between arms is a real
disagreement rather than a different sample.

WHY THESE TYPES
---------------
XPBatch's typed batch contract has a closed, small type set: INT4, INT8,
NUMERIC, VARLENA (xpb_colbatch.h). There is no float and no date. So:

  - int32/int64 only, which is what the contract handles natively;
  - the date is carried as an int32 date KEY (yyyymmdd), not a Parquet DATE32.

That is deliberate and is the task's own priority order: date/timestamp only if
natural for the current typed path, and it is not. Mapping DATE32 to a batch
column would mean inventing a type mapping before anything needs one.

SHAPE
-----
8 row groups of 25 000 rows = 200 000 rows.

  order_date_key  int32  NOT NULL  monotonically increasing across row groups,
                                   each row group covering a disjoint closed
                                   range, so row-group pruning has something
                                   real to prune and the boundaries are exact.
  order_id        int64  NOT NULL  sequential
  customer_id     int32  NOT NULL  order_id % 1000
  amount          int64  NULLABLE  values chosen so sums are exact in int64;
                                   row group 3 is ENTIRELY NULL
  quantity        int32  NULLABLE  carries INT32_MIN / INT32_MAX boundary rows
  status          int32  NOT NULL
  pad_a..pad_d    int64  NOT NULL  never projected by any test query; they
                                   exist so "only projected columns were
                                   decoded" is a claim with teeth. pad_d
                                   carries INT64_MIN / INT64_MAX.

NULLs, an all-NULL row group, and integer boundary values are all present
because each breaks a different thing: validity handling, null_count-based
pruning, and sign handling in min/max statistics.
"""
import csv
import os
import sys

import pyarrow as pa
import pyarrow.parquet as pq

ROW_GROUPS = 8
RG_ROWS = 25_000
NROWS = ROW_GROUPS * RG_ROWS

INT32_MIN, INT32_MAX = -2**31, 2**31 - 1
INT64_MIN, INT64_MAX = -2**63, 2**63 - 1

# Each row group covers exactly 100 consecutive date keys starting at 20260101.
# Keys are synthetic (not real calendar arithmetic) so the ranges stay exact and
# a predicate boundary lands where it is meant to.
DATE_BASE = 20260101
DATES_PER_RG = 100


def build():
    order_date_key, order_id, customer_id = [], [], []
    amount, quantity, status = [], [], []
    pad_a, pad_b, pad_c, pad_d = [], [], [], []

    for i in range(NROWS):
        rg = i // RG_ROWS
        within = i % RG_ROWS

        order_date_key.append(DATE_BASE + rg * DATES_PER_RG + (within % DATES_PER_RG))
        order_id.append(i)
        customer_id.append(i % 1000)

        # amount: row group 3 is entirely NULL; elsewhere every 97th row is NULL.
        if rg == 3 or i % 97 == 0:
            amount.append(None)
        else:
            amount.append(100 + (i % 500))

        # quantity: boundary values at two known rows, NULL every 1000th.
        if i == 5:
            quantity.append(INT32_MAX)
        elif i == 7:
            quantity.append(INT32_MIN)
        elif i % 1000 == 500:
            quantity.append(None)
        else:
            quantity.append(1 + (i % 9))

        status.append(i % 4)
        pad_a.append(i * 3)
        pad_b.append(-i)
        pad_c.append(i % 17)
        pad_d.append(INT64_MAX if i == 11 else (INT64_MIN if i == 13 else i))

    return pa.table({
        "order_date_key": pa.array(order_date_key, type=pa.int32()),
        "order_id":       pa.array(order_id,       type=pa.int64()),
        "customer_id":    pa.array(customer_id,    type=pa.int32()),
        "amount":         pa.array(amount,         type=pa.int64()),
        "quantity":       pa.array(quantity,       type=pa.int32()),
        "status":         pa.array(status,         type=pa.int32()),
        "pad_a":          pa.array(pad_a,          type=pa.int64()),
        "pad_b":          pa.array(pad_b,          type=pa.int64()),
        "pad_c":          pa.array(pad_c,          type=pa.int64()),
        "pad_d":          pa.array(pad_d,          type=pa.int64()),
    })


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(outdir, exist_ok=True)
    tbl = build()

    parquet_path = os.path.join(outdir, "orders.parquet")
    # PLAIN + uncompressed keeps the file readable by a minimal decoder and
    # keeps "bytes read" interpretable. Statistics ON is the point: pruning may
    # use explicit metadata only, so the metadata has to be there.
    pq.write_table(
        tbl, parquet_path,
        row_group_size=RG_ROWS,
        compression="none",
        use_dictionary=False,
        write_statistics=True,
        version="2.6",
    )

    # Same rows, same row-group layout, statistics DELIBERATELY ABSENT.
    #
    # This is the negative test for the pruning rule: with no declared bounds
    # the only admissible behaviour is to read every row group. A file like this
    # is what turns "skip only on explicit metadata" from a comment into
    # something a test can fail on.
    nostats_path = os.path.join(outdir, "orders_nostats.parquet")
    pq.write_table(
        tbl, nostats_path,
        row_group_size=RG_ROWS,
        compression="none",
        use_dictionary=False,
        write_statistics=False,
        version="2.6",
    )

    csv_path = os.path.join(outdir, "orders.csv")
    with open(csv_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(tbl.column_names)
        cols = [c.to_pylist() for c in tbl.columns]
        for r in range(tbl.num_rows):
            w.writerow(["" if cols[c][r] is None else cols[c][r]
                        for c in range(len(cols))])

    with open(os.path.join(outdir, "schema.sql"), "w") as f:
        f.write("""-- PostgreSQL side of the oracle: the same logical rows as orders.parquet.
-- NOT NULL mirrors the Parquet schema exactly, so a nullability disagreement
-- shows up as a load failure rather than as a wrong answer later.
DROP TABLE IF EXISTS pq_orders;
CREATE TABLE pq_orders (
    order_date_key  int4   NOT NULL,
    order_id        int8   NOT NULL,
    customer_id     int4   NOT NULL,
    amount          int8,
    quantity        int4,
    status          int4   NOT NULL,
    pad_a           int8   NOT NULL,
    pad_b           int8   NOT NULL,
    pad_c           int8   NOT NULL,
    pad_d           int8   NOT NULL
);
""")

    ns = pq.ParquetFile(nostats_path).metadata
    print(f"orders_nostats.parquet: rows={ns.num_rows} row_groups={ns.num_row_groups} "
          f"stats_set={ns.row_group(0).column(0).is_stats_set}")

    md = pq.ParquetFile(parquet_path).metadata
    print(f"rows={md.num_rows} row_groups={md.num_row_groups} "
          f"cols={md.num_columns} bytes={os.path.getsize(parquet_path)}")
    for i in range(md.num_row_groups):
        rg = md.row_group(i)
        c0 = rg.column(0)              # order_date_key
        c3 = rg.column(3)              # amount
        print(f"  rg{i}: rows={rg.num_rows} "
              f"date_key[{c0.statistics.min}..{c0.statistics.max}] "
              f"amount_nulls={c3.statistics.null_count} "
              f"stats_set={c0.is_stats_set}")


if __name__ == "__main__":
    main()
