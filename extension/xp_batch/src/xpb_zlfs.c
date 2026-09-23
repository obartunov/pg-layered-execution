/*
 * xpb_zlfs.c — ZLFS v0.2: generalized file-backed analytical zones
 *
 * No hardcoded table names, column names, or byte offsets.
 * Caller provides relid, attnos[], predicate range.
 * Byte offsets computed from TupleDesc at build time.
 * Schema hash stored and validated at load time.
 */
#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "catalog/namespace.h"
#include "portability/instr_time.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "miscadmin.h"
#include "storage/fd.h"

#include "xpb_zlfs.h"
#include "utils/tuplestore.h"

#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>

#define ZLFS_DIR "zlfs"

ZlfsRegistry *zlfs_reg = NULL;
static bool zlfs_files_scanned = false;

/* ── Schema fingerprint ── */

uint32
zlfs_compute_schema_hash(Oid relid, int16 *attnos, int ncols)
{
    uint32 h = 0x811c9dc5u;  /* FNV-1a seed */
    Relation rel = table_open(relid, AccessShareLock);
    TupleDesc td = RelationGetDescr(rel);

    for (int i = 0; i < ncols; i++)
    {
        int16 attno = attnos[i];
        if (attno < 1 || attno > td->natts)
            ereport(ERROR, (errmsg("ZLFS: attno %d out of range (1..%d)", attno, td->natts)));

        Form_pg_attribute attr = TupleDescAttr(td, attno - 1);

        if (attr->attisdropped)
            ereport(ERROR, (errmsg("ZLFS: attno %d is a dropped column", attno)));

        /* Full attribute fingerprint */
        h ^= (uint32)attno;            h *= 0x01000193u;
        h ^= (uint32)attr->atttypid;   h *= 0x01000193u;
        h ^= (uint32)attr->attlen;     h *= 0x01000193u;
        h ^= (uint32)attr->atttypmod;  h *= 0x01000193u;
        h ^= (uint32)attr->attalign;   h *= 0x01000193u;
        h ^= (uint32)attr->attbyval;   h *= 0x01000193u;
        h ^= (uint32)attr->attnotnull; h *= 0x01000193u;
    }
    table_close(rel, AccessShareLock);
    return h;
}

/* Compute byte offsets for given attnos in a fixed-width NOT NULL tuple.
 * Uses att_align_nominal for correct alignment handling. */
static void
zlfs_compute_offsets(Oid relid, int16 *attnos, int ncols, int *offsets_out)
{
    Relation rel = table_open(relid, AccessShareLock);
    TupleDesc td = RelationGetDescr(rel);

    for (int i = 0; i < ncols; i++)
    {
        int16 attno = attnos[i];
        if (attno < 1 || attno > td->natts)
            ereport(ERROR, (errmsg("ZLFS: attno %d out of range (1..%d)", attno, td->natts)));
        Form_pg_attribute attr = TupleDescAttr(td, attno - 1);

        if (attr->attlen < 1)
            ereport(ERROR, (errmsg("ZLFS: column attno %d has variable length (%d), "
                                    "only fixed-width supported",
                                    attno, attr->attlen)));
        if (!attr->attnotnull)
            ereport(ERROR, (errmsg("ZLFS: column attno %d is nullable", attno)));

        /* Walk preceding attributes with alignment */
        int offset = 0;
        for (int a = 0; a < attno - 1; a++)
        {
            Form_pg_attribute prev = TupleDescAttr(td, a);
            if (prev->attlen < 1)
                ereport(ERROR, (errmsg("ZLFS: preceding column attno %d has variable length, "
                                        "cannot compute fixed offset for attno %d",
                                        a + 1, attno)));
            offset = att_align_nominal(offset, prev->attalign);
            offset += prev->attlen;
        }
        /* Align the target column itself */
        offset = att_align_nominal(offset, attr->attalign);
        offsets_out[i] = offset;
    }
    table_close(rel, AccessShareLock);
}

/* ── Registry ── */

void
zlfs_ensure_registry(void)
{
    if (zlfs_reg) return;
    MemoryContext mcxt = AllocSetContextCreate(TopMemoryContext,
                                               "ZLFS Registry",
                                               ALLOCSET_DEFAULT_SIZES);
    zlfs_reg = MemoryContextAllocZero(mcxt, sizeof(ZlfsRegistry));
    zlfs_reg->mcxt = mcxt;
}

/* ── File I/O ── */

static void
zlfs_write_file(ZlfsZone *zone)
{
    char tmppath[MAXPGPATH];
    snprintf(tmppath, sizeof(tmppath), "%s.tmp.%d", zone->filepath, MyProcPid);
    FILE *f = fopen(tmppath, "wb");
    if (!f)
        ereport(ERROR, (errmsg("ZLFS: cannot open %s for write", tmppath)));

    ZlfsFileHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = ZLFS_MAGIC;
    hdr.version = ZLFS_VERSION;
    hdr.source_relid = zone->source_relid;
    hdr.pred_lo = zone->pred_lo;
    hdr.pred_hi = zone->pred_hi;
    hdr.ncols = zone->ncols;
    hdr.nrows = zone->nrows;
    hdr.col_width = sizeof(int32);
    hdr.build_time = zone->build_time;
    hdr.freshness = (uint32)zone->freshness;
    hdr.schema_hash = zone->schema_hash;
    memcpy(hdr.col_attnos, zone->col_attnos, sizeof(hdr.col_attnos));

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) goto write_err;
    for (int c = 0; c < zone->ncols; c++)
    {
        if (fwrite(zone->cols[c], sizeof(int32), zone->nrows, f) != (size_t)zone->nrows)
            goto write_err;
    }
    if (fflush(f) != 0) goto write_err;
    if (pg_fsync(fileno(f)) != 0)
        elog(WARNING, "ZLFS: fsync failed for %s", tmppath);
    if (fclose(f) != 0) goto write_err;

    if (rename(tmppath, zone->filepath) != 0)
        ereport(ERROR, (errmsg("ZLFS: cannot rename %s → %s", tmppath, zone->filepath)));
    return;

write_err:
    fclose(f);
    unlink(tmppath);
    ereport(ERROR, (errmsg("ZLFS: write error on %s", tmppath)));
}

static void
zlfs_update_freshness(ZlfsZone *zone)
{
    int fd = BasicOpenFile(zone->filepath, O_RDWR | PG_BINARY);
    if (fd < 0) return;

    struct flock lk = {0};
    lk.l_type = F_WRLCK; lk.l_whence = SEEK_SET;
    lk.l_start = 0; lk.l_len = sizeof(ZlfsFileHeader);
    (void) fcntl(fd, F_SETLKW, &lk);

    off_t off = offsetof(ZlfsFileHeader, freshness);
    uint32 val = (uint32)zone->freshness;
    if (pg_pwrite(fd, &val, sizeof(val), off) != sizeof(val))
        elog(WARNING, "ZLFS: failed to update freshness for %s", zone->filepath);
    pg_fsync(fd);

    lk.l_type = F_UNLCK;
    (void) fcntl(fd, F_SETLK, &lk);
    close(fd);
}

static ZlfsZone *
zlfs_load_file(const char *filepath)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) return NULL;

    ZlfsFileHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return NULL; }
    if (hdr.magic != ZLFS_MAGIC) { fclose(f); return NULL; }
    if (hdr.version != ZLFS_VERSION) { fclose(f); return NULL; }
    if (hdr.ncols > ZLFS_MAX_COLS) { fclose(f); return NULL; }

    MemoryContext old = MemoryContextSwitchTo(zlfs_reg->mcxt);
    ZlfsZone *z = palloc0(sizeof(ZlfsZone));
    z->source_relid = (Oid)hdr.source_relid;
    z->pred_lo = hdr.pred_lo;
    z->pred_hi = hdr.pred_hi;
    z->ncols = hdr.ncols;
    z->nrows = hdr.nrows;
    z->freshness = (ZlfsFreshness)hdr.freshness;
    z->build_time = (TimestampTz)hdr.build_time;
    z->schema_hash = hdr.schema_hash;
    memcpy(z->col_attnos, hdr.col_attnos, sizeof(z->col_attnos));
    snprintf(z->filepath, sizeof(z->filepath), "%s", filepath);
    z->mcxt = zlfs_reg->mcxt;

    for (int c = 0; c < (int)hdr.ncols; c++)
    {
        z->cols[c] = palloc(hdr.nrows * sizeof(int32));
        if (fread(z->cols[c], sizeof(int32), hdr.nrows, f) != (size_t)hdr.nrows)
        {
            fclose(f);
            MemoryContextSwitchTo(old);
            return NULL;
        }
    }
    fclose(f);
    MemoryContextSwitchTo(old);
    return z;
}

void
zlfs_scan_directory(void)
{
    if (zlfs_files_scanned) return;
    zlfs_files_scanned = true;

    char dirpath[MAXPGPATH];
    snprintf(dirpath, sizeof(dirpath), "%s/%s", DataDir, ZLFS_DIR);
    DIR *d = opendir(dirpath);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL)
    {
        if (strstr(ent->d_name, ".zlfs") == NULL) continue;
        char fpath[MAXPGPATH];
        snprintf(fpath, sizeof(fpath), "%s/%s", dirpath, ent->d_name);

        /* Skip if already loaded */
        bool found = false;
        for (int i = 0; i < zlfs_reg->nzones; i++)
        {
            if (strcmp(zlfs_reg->zones[i]->filepath, fpath) == 0)
            { found = true; break; }
        }
        if (found) continue;

        ZlfsZone *z = zlfs_load_file(fpath);
        if (z && zlfs_reg->nzones < ZLFS_MAX_ZONES)
        {
            /*
             * Validate schema hash against current catalog.
             *
             * Nothing may leave this block by continue/break/return/goto:
             * PG_END_TRY() is what pops PG_exception_stack and
             * error_context_stack, and skipping it leaves them pointing into
             * this frame after it is gone, so the NEXT ereport(ERROR) anywhere
             * in the session longjmps into a dead frame.  Two `continue`s used
             * to do exactly that; the symptom was a SIGSEGV in a later,
             * unrelated call in the same backend (a stale zone file left by a
             * dropped table was enough to arm it).  Record the verdict, leave
             * the block normally, act afterwards.
             *
             * The catch arm also restores CurrentMemoryContext: on longjmp it
             * is whatever the thrower left current, and allocations made after
             * that land in a context this loop does not own.
             */
            bool    zone_usable = true;
            bool    validate_failed = false;
            uint32  current_hash = 0;

            if (OidIsValid(z->source_relid) && z->ncols > 0)
            {
                MemoryContext caller_cx = CurrentMemoryContext;

                PG_TRY();
                {
                    current_hash = zlfs_compute_schema_hash(
                        z->source_relid, z->col_attnos, z->ncols);
                }
                PG_CATCH();
                {
                    /* the source table may have been dropped since the build */
                    MemoryContextSwitchTo(caller_cx);
                    FlushErrorState();
                    validate_failed = true;
                }
                PG_END_TRY();

                if (validate_failed)
                {
                    elog(WARNING, "ZLFS: cannot validate schema for %s "
                         "(source relation gone?), skipping", fpath);
                    zone_usable = false;
                }
                else if (current_hash != z->schema_hash)
                {
                    elog(WARNING, "ZLFS: schema mismatch for %s "
                         "(stored=%08x, current=%08x), skipping",
                         fpath, z->schema_hash, current_hash);
                    zone_usable = false;
                }
            }

            if (!zone_usable)
                continue;       /* outside the TRY block: legal */

            zlfs_reg->zones[zlfs_reg->nzones++] = z;
        }
    }
    closedir(d);
}

ZlfsZone *
zlfs_lookup_valid_zone(Oid relid, int32 pred_lo, int32 pred_hi)
{
    if (!zlfs_reg) return NULL;
    for (int i = 0; i < zlfs_reg->nzones; i++)
    {
        ZlfsZone *z = zlfs_reg->zones[i];
        if (z->source_relid == relid &&
            z->pred_lo == pred_lo && z->pred_hi == pred_hi &&
            z->freshness == ZLFS_VALID)
            return z;
    }
    return NULL;
}

/* ── SQL: zlfs_build_zone(relname, col_attnos_csv, lo, hi) ── */

PG_FUNCTION_INFO_V1(zlfs_build_zone);

Datum
zlfs_build_zone(PG_FUNCTION_ARGS)
{
    char *relname = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char *cols_csv = text_to_cstring(PG_GETARG_TEXT_PP(1));
    int32 lo = PG_GETARG_INT32(2);
    int32 hi = PG_GETARG_INT32(3);

    Oid relid = RelnameGetRelid(relname);
    if (!OidIsValid(relid))
        ereport(ERROR, (errmsg("table %s not found", relname)));

    /* Parse comma-separated attnos */
    int16 attnos[ZLFS_MAX_COLS];
    int ncols = 0;
    char *p = cols_csv;
    while (*p && ncols < ZLFS_MAX_COLS)
    {
        attnos[ncols++] = (int16)atoi(p);
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
    if (ncols < 1)
        ereport(ERROR, (errmsg("ZLFS: need at least 1 column")));

    /* Compute offsets and schema hash */
    int offsets[ZLFS_MAX_COLS];
    zlfs_compute_offsets(relid, attnos, ncols, offsets);
    uint32 schema_hash = zlfs_compute_schema_hash(relid, attnos, ncols);

    zlfs_ensure_registry();
    zlfs_scan_directory();

    /* Evict existing zone for same range */
    for (int i = 0; i < zlfs_reg->nzones; i++)
    {
        ZlfsZone *z = zlfs_reg->zones[i];
        if (z->source_relid == relid && z->pred_lo == lo && z->pred_hi == hi)
        {
            zlfs_reg->zones[i] = zlfs_reg->zones[--zlfs_reg->nzones];
            break;
        }
    }

    instr_time t0, t1;
    INSTR_TIME_SET_CURRENT(t0);

    /* Allocate zone */
    MemoryContext old = MemoryContextSwitchTo(zlfs_reg->mcxt);
    ZlfsZone *zone = palloc0(sizeof(ZlfsZone));
    zone->source_relid = relid;
    zone->pred_lo = lo;
    zone->pred_hi = hi;
    zone->ncols = ncols;
    zone->schema_hash = schema_hash;
    memcpy(zone->col_attnos, attnos, ncols * sizeof(int16));
    memcpy(zone->col_offsets, offsets, ncols * sizeof(int));
    zone->mcxt = zlfs_reg->mcxt;

    int64 capacity = 2000000;
    for (int c = 0; c < ncols; c++)
        zone->cols[c] = palloc(capacity * sizeof(int32));
    MemoryContextSwitchTo(old);

    /* Scan heap */
    Relation rel = table_open(relid, AccessShareLock);
    Snapshot snap = GetActiveSnapshot();
    BlockNumber nblocks = RelationGetNumberOfBlocks(rel);
    Oid rel_oid = RelationGetRelid(rel);
    Buffer vmbuf = InvalidBuffer;
    int64 nrows = 0;

    for (BlockNumber blkno = 0; blkno < nblocks; blkno++)
    {
        Buffer buf = ReadBufferExtended(rel, MAIN_FORKNUM, blkno, RBM_NORMAL, NULL);
        LockBuffer(buf, BUFFER_LOCK_SHARE);
        Page page = BufferGetPage(buf);
        bool all_visible = (visibilitymap_get_status(rel, blkno, &vmbuf) &
                            VISIBILITYMAP_ALL_VISIBLE) != 0;
        OffsetNumber maxoff = PageGetMaxOffsetNumber(page);

        for (OffsetNumber off = FirstOffsetNumber; off <= maxoff; off++)
        {
            ItemId lp = PageGetItemId(page, off);
            if (!ItemIdIsNormal(lp)) continue;
            HeapTupleHeader htup = (HeapTupleHeader) PageGetItem(page, lp);

            if (!all_visible)
            {
                HeapTupleData td;
                td.t_data = htup; td.t_len = ItemIdGetLength(lp);
                td.t_tableOid = rel_oid;
                ItemPointerSet(&td.t_self, blkno, off);
                if (!HeapTupleSatisfiesVisibility(&td, snap, buf)) continue;
            }

            if (htup->t_infomask & HEAP_HASNULL)
                ereport(ERROR, (errmsg("ZLFS build: tuple has null bitmap")));

            char *d = (char *)htup + htup->t_hoff;
            int32 pred_val = *(int32 *)(d + offsets[0]);
            if (pred_val < lo || pred_val > hi) continue;

            /* Grow if needed */
            if (nrows >= capacity)
            {
                int64 newcap = capacity * 2;
                MemoryContext o2 = MemoryContextSwitchTo(zlfs_reg->mcxt);
                for (int c = 0; c < ncols; c++)
                    zone->cols[c] = repalloc(zone->cols[c], newcap * sizeof(int32));
                MemoryContextSwitchTo(o2);
                capacity = newcap;
            }

            for (int c = 0; c < ncols; c++)
                zone->cols[c][nrows] = *(int32 *)(d + offsets[c]);
            nrows++;
        }
        LockBuffer(buf, BUFFER_LOCK_UNLOCK);
        ReleaseBuffer(buf);
    }
    if (vmbuf != InvalidBuffer) ReleaseBuffer(vmbuf);
    table_close(rel, AccessShareLock);

    zone->nrows = nrows;
    zone->freshness = ZLFS_VALID;
    zone->build_time = GetCurrentTimestamp();

    /* Write to disk */
    char dirpath[MAXPGPATH];
    snprintf(dirpath, sizeof(dirpath), "%s/%s", DataDir, ZLFS_DIR);
    mkdir(dirpath, S_IRWXU);
    snprintf(zone->filepath, sizeof(zone->filepath),
             "%s/zone_%u_%d_%d.zlfs", dirpath, relid, lo, hi);
    zlfs_write_file(zone);

    /* Register */
    if (zlfs_reg->nzones < ZLFS_MAX_ZONES)
        zlfs_reg->zones[zlfs_reg->nzones++] = zone;

    INSTR_TIME_SET_CURRENT(t1);
    double build_ms = INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

    elog(NOTICE, "ZLFS zone [%d..%d] built: %ld rows, %.0f ms, %ld KB, "
         "ncols=%d, schema_hash=%08x, file=%s",
         lo, hi, nrows, build_ms, nrows * ncols * 4 / 1024,
         ncols, schema_hash, zone->filepath);

    PG_RETURN_TEXT_P(cstring_to_text("VALID"));
}

/* ── SQL: zlfs_invalidate_zone(lo, hi) ── */

PG_FUNCTION_INFO_V1(zlfs_invalidate_zone);

Datum
zlfs_invalidate_zone(PG_FUNCTION_ARGS)
{
    int32 lo = PG_GETARG_INT32(0);
    int32 hi = PG_GETARG_INT32(1);

    zlfs_ensure_registry();
    zlfs_scan_directory();

    for (int i = 0; i < zlfs_reg->nzones; i++)
    {
        ZlfsZone *z = zlfs_reg->zones[i];
        if (z->pred_lo == lo && z->pred_hi == hi && z->freshness == ZLFS_VALID)
        {
            z->freshness = ZLFS_STALE;
            zlfs_update_freshness(z);
            PG_RETURN_TEXT_P(cstring_to_text("STALE"));
        }
    }
    PG_RETURN_TEXT_P(cstring_to_text("NOT_FOUND"));
}

/* ── SQL: zlfs_drop_zone(lo, hi) ── */

PG_FUNCTION_INFO_V1(zlfs_drop_zone);

Datum
zlfs_drop_zone(PG_FUNCTION_ARGS)
{
    int32 lo = PG_GETARG_INT32(0);
    int32 hi = PG_GETARG_INT32(1);

    zlfs_ensure_registry();
    zlfs_scan_directory();

    for (int i = 0; i < zlfs_reg->nzones; i++)
    {
        ZlfsZone *z = zlfs_reg->zones[i];
        if (z->pred_lo == lo && z->pred_hi == hi)
        {
            unlink(z->filepath);
            zlfs_reg->zones[i] = zlfs_reg->zones[--zlfs_reg->nzones];
            PG_RETURN_VOID();
        }
    }
    PG_RETURN_VOID();
}

/* ── SQL: zlfs_zone_info() ── */

PG_FUNCTION_INFO_V1(zlfs_zone_info);

Datum
zlfs_zone_info(PG_FUNCTION_ARGS)
{
    zlfs_ensure_registry();
    zlfs_scan_directory();

    InitMaterializedSRF(fcinfo, 0);
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

    for (int i = 0; i < zlfs_reg->nzones; i++)
    {
        ZlfsZone *z = zlfs_reg->zones[i];
        Datum vals[7];
        bool nulls[7] = {false};

        vals[0] = Int32GetDatum(z->pred_lo);
        vals[1] = Int32GetDatum(z->pred_hi);
        vals[2] = Int64GetDatum(z->nrows);
        vals[3] = Int64GetDatum(z->nrows * z->ncols * 4 / 1024);
        vals[4] = CStringGetTextDatum(
            z->freshness == ZLFS_VALID ? "VALID" :
            z->freshness == ZLFS_STALE ? "STALE" : "MISSING");
        vals[5] = TimestampTzGetDatum(z->build_time);
        vals[6] = CStringGetTextDatum(z->filepath);

        tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, vals, nulls);
    }
    return (Datum) 0;
}

/* ── SQL: zlfs_group_sum(relname, cols_csv, lo, hi) — backward compat SRF ── */

PG_FUNCTION_INFO_V1(zlfs_group_sum);

Datum
zlfs_group_sum(PG_FUNCTION_ARGS)
{
    int32 lo = PG_GETARG_INT32(0);
    int32 hi = PG_GETARG_INT32(1);

    /* Backward compat: hardcoded to reg_buh cols 1,2,6 */
    Oid relid = RelnameGetRelid("reg_buh");
    if (!OidIsValid(relid))
        ereport(ERROR, (errmsg("table reg_buh not found")));

    zlfs_ensure_registry();
    zlfs_scan_directory();
    ZlfsZone *zone = zlfs_lookup_valid_zone(relid, lo, hi);

    InitMaterializedSRF(fcinfo, 0);
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

    if (!zone || zone->freshness != ZLFS_VALID)
    {
        /* Heap fallback omitted for brevity — use xpb_batch_groupby(lo,hi,'heap') */
        ereport(ERROR, (errmsg("ZLFS: no valid zone, use xpb_batch_groupby for heap fallback")));
    }

    /* ZLFS scan path — find columns by attno */
    int pk_idx = -1, ck_idx = -1, dt_idx = -1;
    for (int c = 0; c < zone->ncols; c++)
    {
        if (zone->col_attnos[c] == 1) pk_idx = c;  /* period_key = attno 1 */
        if (zone->col_attnos[c] == 2) ck_idx = c;  /* company_key = attno 2 */
        if (zone->col_attnos[c] == 6) dt_idx = c;  /* amount_dt = attno 6 */
    }
    if (pk_idx < 0 || ck_idx < 0 || dt_idx < 0)
        ereport(ERROR, (errmsg("ZLFS: zone missing required columns")));

    instr_time t0, t1;
    INSTR_TIME_SET_CURRENT(t0);

    #define GRP_CAP 16384
    #define GRP_MAX_LOAD (GRP_CAP * 3 / 4)
    typedef struct { int32 k1, k2; int64 sum; bool occ; } GrpEntry;
    GrpEntry *ht = palloc0(GRP_CAP * sizeof(GrpEntry));
    int ngroups = 0;

    int32 *col_pk = zone->cols[pk_idx];
    int32 *col_ck = zone->cols[ck_idx];
    int32 *col_dt = zone->cols[dt_idx];

    for (int64 i = 0; i < zone->nrows; i++)
    {
        int32 pk = col_pk[i], ck = col_ck[i], dt = col_dt[i];
        uint32 h = (uint32)pk * 2654435761u ^ (uint32)ck * 2246822519u;
        int sl = (int)(h & (GRP_CAP - 1));
        for (int pr = 0; pr < GRP_CAP; pr++)
        {
            int idx = (sl + pr) & (GRP_CAP - 1);
            if (!ht[idx].occ) {
                if (ngroups >= GRP_MAX_LOAD)
                    ereport(ERROR, (errmsg("ZLFS: hash overflow")));
                ht[idx].k1=pk; ht[idx].k2=ck; ht[idx].sum=dt; ht[idx].occ=true;
                ngroups++; break;
            }
            if (ht[idx].k1==pk && ht[idx].k2==ck) { ht[idx].sum+=dt; break; }
        }
    }

    INSTR_TIME_SET_CURRENT(t1);
    double scan_ms = INSTR_TIME_GET_MILLISEC(t1) - INSTR_TIME_GET_MILLISEC(t0);

    Datum vals[4]; bool nulls[4] = {false};
    for (int i = 0; i < GRP_CAP; i++) {
        if (!ht[i].occ) continue;
        vals[0] = Int32GetDatum(ht[i].k1);
        vals[1] = Int32GetDatum(ht[i].k2);
        vals[2] = Int64GetDatum(ht[i].sum);
        vals[3] = Float8GetDatum(scan_ms);
        tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, vals, nulls);
    }

    elog(NOTICE, "ZLFS: scan=%.1f ms  zone=[%d..%d]  rows=%ld  groups=%d",
         scan_ms, lo, hi, zone->nrows, ngroups);

    pfree(ht);
    return (Datum) 0;
}
