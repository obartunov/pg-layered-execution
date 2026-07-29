/*
 * xpb_columnar_experiment.c
 *
 * Measures carrier cost: row structs vs columnar arrays.
 * Three paths:
 *   A) CGroupEntry struct array (current xp_batch output format)
 *   B) Dense column arrays (ColumnarTR)
 *   C) Tuple + Datum emission (simulating tuplestore read overhead)
 */
#include "postgres.h"
#include "funcapi.h"
#include "portability/instr_time.h"
#include "utils/builtins.h"
#include "miscadmin.h"

#include "xpb_columnar_tr.h"

PG_FUNCTION_INFO_V1(ctr_experiment);

typedef struct RowEntry {
    int64 k1, k2, sum_val, count;
} RowEntry;

Datum
ctr_experiment(PG_FUNCTION_ARGS)
{
    int64 ngroups    = PG_GETARG_INT64(0);
    int   iterations = PG_GETARG_INT32(1);
    StringInfoData buf;
    instr_time t0, t1;
    double row_write_ms = 0, row_read_ms = 0;
    double col_write_ms = 0, col_read_ms = 0;
    double datum_read_ms = 0;
    volatile int64 sink = 0;  /* prevent optimization */

    initStringInfo(&buf);

    for (int iter = 0; iter < iterations; iter++)
    {
        /* ── Path A: Struct array (simulates CGroupEntry[]) ── */
        RowEntry *rows = palloc(ngroups * sizeof(RowEntry));

        INSTR_TIME_SET_CURRENT(t0);
        for (int64 i = 0; i < ngroups; i++)
        {
            rows[i].k1 = i / 50;
            rows[i].k2 = i % 50;
            rows[i].sum_val = i * 1000 + 7;
            rows[i].count = i * 3 + 1;
        }
        INSTR_TIME_SET_CURRENT(t1);
        row_write_ms += INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

        INSTR_TIME_SET_CURRENT(t0);
        {
            int64 s = 0;
            for (int64 i = 0; i < ngroups; i++)
                s += rows[i].sum_val;
            sink += s;
        }
        INSTR_TIME_SET_CURRENT(t1);
        row_read_ms += INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

        /* ── Path B: Columnar arrays ── */
        int64 *c0 = palloc(ngroups * sizeof(int64));
        int64 *c1 = palloc(ngroups * sizeof(int64));
        int64 *c2 = palloc(ngroups * sizeof(int64));
        int64 *c3 = palloc(ngroups * sizeof(int64));

        INSTR_TIME_SET_CURRENT(t0);
        for (int64 i = 0; i < ngroups; i++)
        {
            c0[i] = i / 50;
            c1[i] = i % 50;
            c2[i] = i * 1000 + 7;
            c3[i] = i * 3 + 1;
        }
        INSTR_TIME_SET_CURRENT(t1);
        col_write_ms += INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

        INSTR_TIME_SET_CURRENT(t0);
        {
            int64 s = 0;
            for (int64 i = 0; i < ngroups; i++)
                s += c2[i];
            sink += s;
        }
        INSTR_TIME_SET_CURRENT(t1);
        col_read_ms += INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

        /* ── Path C: Datum emission (simulates slot_getattr overhead) ── */
        INSTR_TIME_SET_CURRENT(t0);
        {
            int64 s = 0;
            for (int64 i = 0; i < ngroups; i++)
            {
                /* Simulate: slot->tts_values[2] = Int64GetDatum(sum_val) */
                Datum d = Int64GetDatum(rows[i].sum_val);
                /* Simulate: DatumGetInt64(slot_getattr(...)) */
                s += DatumGetInt64(d);
            }
            sink += s;
        }
        INSTR_TIME_SET_CURRENT(t1);
        datum_read_ms += INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

        pfree(rows);
        pfree(c0); pfree(c1); pfree(c2); pfree(c3);
    }

    appendStringInfo(&buf,
        "ngroups=%ld iters=%d "
        "| struct: write=%.2f read=%.2f total=%.2f ms "
        "| column: write=%.2f read=%.2f total=%.2f ms "
        "| datum_read=%.2f ms "
        "| col/struct_read=%.2fx "
        "| struct_bytes=%ld col_bytes=%ld "
        "| sink=%ld",
        ngroups, iterations,
        row_write_ms / iterations, row_read_ms / iterations,
        (row_write_ms + row_read_ms) / iterations,
        col_write_ms / iterations, col_read_ms / iterations,
        (col_write_ms + col_read_ms) / iterations,
        datum_read_ms / iterations,
        col_read_ms > 0 ? row_read_ms / col_read_ms : 0,
        ngroups * (int64)sizeof(RowEntry),
        ngroups * 4 * (int64)sizeof(int64),
        sink);

    PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}
