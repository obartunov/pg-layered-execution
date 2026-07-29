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
#define ZLFS_VERSION    2

typedef enum ZlfsFreshness {
    ZLFS_MISSING = 0,
    ZLFS_VALID   = 1,
    ZLFS_STALE   = 2
} ZlfsFreshness;

/*
 * On-disk file header (128 bytes, fixed).
 * Followed by ncols × nrows × 4 bytes of int32 column data.
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
    uint32          col_width;      /* sizeof(int32) */
    int64           build_time;     /* TimestampTz */
    uint32          freshness;

    /* Schema fingerprint: attno list + type hash for validation */
    int16           col_attnos[ZLFS_MAX_COLS];
    uint32          schema_hash;    /* hash of (attnos + types) */

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
    int             col_offsets[ZLFS_MAX_COLS];  /* byte offset in tuple data */
    uint32          schema_hash;

    int32          *cols[ZLFS_MAX_COLS];
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
