/*
 * xpb_parquet_shim.h — C ABI over the Parquet C++ reader.
 *
 * No Arrow or Parquet type appears here. The C side of the module includes
 * this and nothing else, so PostgreSQL headers and Arrow headers never meet in
 * one translation unit -- which matters because both are large and because
 * PostgreSQL's macros and C++ standard headers disagree about several
 * identifiers.
 *
 * The shim deliberately does NOT know about XpColumnBatch. It decodes a row
 * group's selected columns and reports where the values are; the C provider
 * decides how to present them as a batch. Keeping the batch contract out of
 * the C++ half means the ownership boundary has exactly one crossing.
 */
#ifndef XPB_PARQUET_SHIM_H
#define XPB_PARQUET_SHIM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XpqReader XpqReader;

/*
 * What the shim can hand over. Mirrors the batch contract's integer types;
 * anything else in the file is reported as UNSUPPORTED and the provider
 * refuses rather than guessing a conversion.
 */
typedef enum XpqColType
{
    XPQ_COL_UNSUPPORTED = 0,
    XPQ_COL_INT32,
    XPQ_COL_INT64
} XpqColType;

/*
 * One decoded column of one row group.
 *
 * `data` points at `nrows` values of 4 or 8 bytes. `validity` is NULL when the
 * column has no nulls in this row group -- the same "NULL means all valid"
 * convention the batch contract uses, so the provider passes it straight
 * through. When non-NULL it is a bit-per-row map with SET MEANING VALID, which
 * is also Arrow's own convention, so no inversion happens anywhere.
 *
 * Both pointers are owned by the reader and stay valid until the next
 * xpq_read_row_group() or xpq_close() on the same reader. That window is
 * deliberately identical to the batch contract's borrow window.
 */
typedef struct XpqColumn
{
    XpqColType      type;
    const void     *data;
    const uint8_t  *validity;
    int64_t         nrows;
    int64_t         null_count;
    int             copied;     /* 1 when values were copied, not borrowed */
} XpqColumn;

/*
 * Open a Parquet file and read only its footer. Returns NULL and fills errbuf
 * on failure. No C++ exception crosses this ABI.
 */
XpqReader  *xpq_open(const char *path, char *errbuf, size_t errbuflen);
void        xpq_close(XpqReader *r);

/* File-level metadata, all from the footer. */
int64_t     xpq_num_rows(const XpqReader *r);
int         xpq_num_row_groups(const XpqReader *r);
int         xpq_num_columns(const XpqReader *r);
const char *xpq_column_name(const XpqReader *r, int col);
XpqColType  xpq_column_type(const XpqReader *r, int col);
int         xpq_column_index(const XpqReader *r, const char *name);  /* -1 if absent */

/*
 * Row-group statistics, straight from the footer.
 *
 * Returns 1 ONLY when the writer actually set statistics for that column chunk
 * and min/max are both present. Returns 0 otherwise, and the caller must then
 * treat the row group as one it cannot exclude. There is no inference here and
 * no statistical proxy: either the file states the bounds or it does not.
 */
int         xpq_row_group_stats_i64(const XpqReader *r, int rg, int col,
                                    int64_t *min_out, int64_t *max_out,
                                    int64_t *null_count_out);
int64_t     xpq_row_group_rows(const XpqReader *r, int rg);

/*
 * Decode one row group, only the requested columns.
 *
 * `cols` holds `ncols` file column indices; `out` receives `ncols` entries in
 * the same order. Returns 0 on success, -1 on failure with errbuf filled.
 * Columns not listed are never decoded, which is the claim projection pushdown
 * rests on; xpq_decoded_values() is the evidence.
 */
int         xpq_read_row_group(XpqReader *r, int rg, const int *cols, int ncols,
                               XpqColumn *out, char *errbuf, size_t errbuflen);

/* ── Instrumentation. Required to prove projection and pruning. ── */

/*
 * Compressed bytes of the SELECTED column chunks, summed from the footer's
 * per-chunk total_compressed_size for the row groups actually read.
 *
 * This is PROJECTED COMPRESSED BYTES ATTRIBUTABLE FROM PARQUET METADATA. It is
 * what projection and pruning move, and it is not a measurement of I/O: no
 * syscall is counted, and the page cache is not consulted. Real byte-range
 * accounting is xpq_read_calls/xpq_data_bytes below, counted in the
 * ObjectReader.
 */
int64_t     xpq_attributed_bytes(const XpqReader *r);

/* Whole-file compressed size of one column, all row groups, from the footer.
 * Lets a caller show what a projection did NOT ask for. */
int64_t     xpq_column_compressed_bytes(const XpqReader *r, int col);
int64_t     xpq_decoded_values(const XpqReader *r);   /* values materialised       */
int64_t     xpq_copy_bytes(const XpqReader *r);       /* copied, not borrowed      */
int         xpq_row_groups_read(const XpqReader *r);
int64_t     xpq_chunks_seen(const XpqReader *r);      /* Arrow chunks, summed      */
double      xpq_open_ms(const XpqReader *r);          /* footer read + parse       */
double      xpq_decode_ms(const XpqReader *r);        /* cumulative decode         */

/*
 * Real byte-range accounting, counted in the ObjectReader -- the only layer
 * that sees a physical read. Metadata and data are split at the first
 * column-chunk read, and read_calls counts physical reads because a hundred
 * small ranges and one large one are different economics at equal bytes.
 *
 * These three were defined in the shim and never declared here, so the C module
 * called them under C's implicit-declaration rule -- which assumes they return
 * int. The measured numbers happened to be unaffected (65 536, ~3 MB, 17 all
 * fit in 32 bits), but the instrument would have truncated silently on a larger
 * file, and the return value is int64_t.
 */
int64_t     xpq_meta_bytes(const XpqReader *r);       /* footer + schema bytes     */
int64_t     xpq_data_bytes(const XpqReader *r);       /* column-chunk bytes read   */
int64_t     xpq_read_calls(const XpqReader *r);       /* physical reads            */

/*
 * Straight from the ObjectReader, which is the only layer that sees a physical
 * read. bytes_requested and bytes_returned differ when Arrow coalesces across a
 * gap: it asks for the span and gets the span, including the chunk in between
 * that the projection did not want. meta/data calls are counted now, not
 * derived from byte totals.
 */
int64_t     xpq_bytes_requested(const XpqReader *r);
int64_t     xpq_bytes_returned(const XpqReader *r);
int64_t     xpq_meta_calls(const XpqReader *r);
int64_t     xpq_data_calls(const XpqReader *r);

/*
 * Physical reads whose interrupt check was skipped because the read was on an
 * Arrow worker thread rather than the backend's.
 *
 * Not a diagnostic afterthought: with pre_buffer on, EVERY column-chunk read
 * arrives on a worker, so this is nonzero for any data scan. A test asserts it
 * is nonzero precisely so that "the hook never ran off-thread" cannot pass
 * vacuously in a build where Arrow happened not to use its pool.
 */
int64_t     xpq_interrupt_skipped_offthread(const XpqReader *r);

/*
 * Reads that came back short while the reader did not claim end of object --
 * the case the v0 contract would have accepted as a clean EOF.
 */
int64_t     xpq_short_reads_without_eof(const XpqReader *r);

/*
 * What ARROW asked for, counted at the adapter. The physical counters above are
 * what the reader actually did; these two differ as soon as anything below
 * retries or splits a read, which is how "a retry changes the physical traffic
 * and not the logical request" is shown rather than asserted.
 */
int64_t     xpq_logical_calls(const XpqReader *r);
int64_t     xpq_logical_bytes(const XpqReader *r);

/*
 * Open with a fault injected underneath the Parquet reader.
 *
 * Same file, same bytes, same decode path; only the delivery is damaged, so the
 * answer stays checkable against the PostgreSQL oracle. mode matches
 * xpb::FaultMode: 0 none, 1 short-without-eof, 2 transient-then-succeed,
 * 3 fail-after-n-bytes, 4 genuine EOF, 5 chunked.
 *
 * For tests. Nothing in the normal path calls it, and it is the only way to
 * reach failure semantics without a network.
 */
XpqReader  *xpq_open_faulty(const char *path, int mode, int64_t at_read,
                            int64_t nbytes, int retries,
                            char *errbuf, size_t errbuflen);

/* Physical attempts the faulty reader made against the real file, and faults
 * actually injected. -1 when the reader is not a faulty one. */
int64_t     xpq_faulty_attempts(const XpqReader *r);
int64_t     xpq_faulty_injected(const XpqReader *r);

/*
 * Install the interrupt check used between range reads. Called once from the
 * module's _PG_init; the hook may longjmp.
 */
void        xpq_set_interrupt_hook(void (*hook)(void));

/*
 * Controlled failure for the C/C++ boundary, used by test/parquet_cxx_boundary.sh.
 * kind 1 throws a std::exception, 2 a parquet::ParquetException, 3 something that
 * is not a std::exception at all; 0 throws nothing. Returns -1 with errbuf set
 * when an exception was caught, which is the only way one may leave this ABI.
 */
int         xpq_selftest_throw(int kind, char *errbuf, size_t errbuflen);

#ifdef __cplusplus
}
#endif

#endif  /* XPB_PARQUET_SHIM_H */
