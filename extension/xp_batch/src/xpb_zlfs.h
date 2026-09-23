/*
 * xpb_zlfs.h — ZLFS v0.2: generalized file-backed analytical zones
 *
 * Zones are not hardcoded to any specific table or column set.
 * At build time, the caller specifies relation OID, column attnos,
 * and a predicate range on the first column. The zone stores a
 * TupleDesc fingerprint for validation at load time.
 */
#ifndef XPB_ZLFS_H
#define XPB_ZLFS_H

#include "postgres.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#define ZLFS_MAX_ZONES  256
#define ZLFS_MAX_COLS   8
#define ZLFS_MAGIC      0x5A4C4653  /* "ZLFS" */
/*
 * On-disk format version.
 *
 * NOTE ON NUMBERING.  The typed-contract milestone calls the old int32-only
 * layout "ZLFS v1" and the typed one "ZLFS v2".  The on-disk field does not
 * use those numbers: it already read 2 for the int32-only format, so the
 * typed format is on-disk version 3.  Written out once so the two numbering
 * schemes never get conflated:
 *
 *     milestone name   on-disk version   layout
 *     ZLFS v1          2                 int32 columns, no NULLs
 *     ZLFS v2          3                 typed columns + validity
 *
 * Version 2 files are NOT read.  A zone is a rebuildable cache -- 82 ms for
 * a million rows -- and carrying a second reader for a format with no type
 * or validity information would mean inferring both, which is the failure
 * this milestone exists to remove.  An old file is rejected by name and
 * rebuilt, never reinterpreted.
 */
#define ZLFS_VERSION    3
#define ZLFS_VERSION_UNTYPED 2      /* the int32-only predecessor */

typedef enum ZlfsFreshness {
    ZLFS_MISSING = 0,
    ZLFS_VALID   = 1,
    ZLFS_STALE   = 2
} ZlfsFreshness;

/* Column type codes as written to disk.  Distinct from XpbColType so the
 * on-disk encoding does not move when that enum does. */
typedef enum ZlfsColType
{
    ZLFS_COL_NONE = 0,
    ZLFS_COL_INT4 = 1,
    ZLFS_COL_INT8 = 2
} ZlfsColType;

#define ZLFS_VALIDITY_BYTES(n)  (((size_t)(n) + 7) / 8)
#define ZLFS_COLFLAG_VALIDITY   0x01    /* a validity bitmap follows the data */

/*
 * On-disk file header, followed for each column c in order by:
 *
 *     nrows * col_widths[c] bytes of dense column data
 *     ((nrows + 7) / 8) bytes of validity bitmap, only when
 *         col_flags[c] & ZLFS_COLFLAG_VALIDITY
 *
 * Bit set in the validity bitmap means the row is VALID, matching the batch
 * contract and pgcolumnar's present bitmap, so neither has to invert it.
 *
 * A NULL row's data slot is written as it stands and must not be read; it
 * carries no meaning and is not a sentinel.
 */
typedef struct ZlfsFileHeader
{
    uint32          magic;
    uint32          version;
    uint32          source_relid;
    int32           pred_lo;
    int32           pred_hi;
    uint32          ncols;
    int64           nrows;
    uint32          col_width;      /* v2 only: sizeof(int32). 0 in v3. */
    int64           build_time;     /* TimestampTz */
    uint32          freshness;

    /* Schema fingerprint: attno list + type hash for validation */
    int16           col_attnos[ZLFS_MAX_COLS];
    uint32          schema_hash;    /* hash of (attnos + types) */

    uint8           col_types[ZLFS_MAX_COLS];   /* ZlfsColType */
    uint8           col_widths[ZLFS_MAX_COLS];  /* bytes per value */
    uint8           col_flags[ZLFS_MAX_COLS];   /* ZLFS_COLFLAG_* */

    uint32          _reserved[2];
} ZlfsFileHeader;

/* In-memory zone */
typedef struct ZlfsZone
{
    Oid             source_relid;
    int32           pred_lo;
    int32           pred_hi;

    int             ncols;
    int16           col_attnos[ZLFS_MAX_COLS];  /* 1-based attno */
    uint32          schema_hash;

    /*
     * Typed columns.  cols[c] holds nrows values of col_widths[c] bytes;
     * col_validity[c] is NULL when the column has no NULLs, which is the
     * common case and costs nothing.
     */
    ZlfsColType     col_types[ZLFS_MAX_COLS];
    uint8           col_widths[ZLFS_MAX_COLS];
    void           *cols[ZLFS_MAX_COLS];
    uint8          *col_validity[ZLFS_MAX_COLS];
    int64           nrows;

    ZlfsFreshness   freshness;
    TimestampTz     build_time;
    char            filepath[128];
    MemoryContext    mcxt;
} ZlfsZone;

typedef struct ZlfsRegistry
{
    ZlfsZone       *zones[ZLFS_MAX_ZONES];
    int             nzones;
    MemoryContext    mcxt;
} ZlfsRegistry;

/* Public API — generalized */
extern ZlfsZone *zlfs_lookup_valid_zone(Oid relid, int32 pred_lo, int32 pred_hi);
extern void zlfs_ensure_registry(void);
extern void zlfs_scan_directory(void);
extern ZlfsRegistry *zlfs_reg;

/* Schema fingerprint */
extern uint32 zlfs_compute_schema_hash(Oid relid, int16 *attnos, int ncols);

#endif /* XPB_ZLFS_H */
