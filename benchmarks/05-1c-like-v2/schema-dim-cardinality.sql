-- Dimension Hash Growth v1: a workload where DIMENSION cardinality is the
-- variable and group cardinality is held still.
--
-- 05-E's trick was to hold the fact table byte-identical and vary a dimension
-- *payload*, which changes the group count without touching a single fact row.
-- That cannot be done here, because the thing being varied is the number of
-- distinct dimension *keys*, and every such key has to be referenced by the
-- fact table or its rows would simply drop out of the join. Two honest options
-- existed:
--
--   (a) keep the fact table fixed over the widest key space and populate fewer
--       dimension rows at the small points -- then 90% of rows fail the join at
--       the low end and the phases after the join are measuring row count, not
--       dimension cardinality;
--   (b) regenerate the fact keys per point, keeping the ROW COUNT identical and
--       every key covered.
--
-- (b) is used. The deviation from "fact data stays byte-identical" is
-- deliberate and is the only way to vary dimension cardinality with full key
-- coverage. What is held constant instead, at every point:
--
--   input rows            1 032 192, always
--   rows surviving joins  1 032 192, always (every key is present)
--   group cardinality     C * G = 12 288, always
--   period range          1..12, always
--   fact table layout     identical, fixed-width NOT NULL prefix
--
-- Group cardinality is pinned at 12 288 on purpose: that is a cardinality the
-- growing group hash is already measured to handle normally (one growth, load
-- 0.375), so the group hash cannot be what changes across this ladder. Only the
-- two dimension hashes move.
--
-- The ladder, and why these points:
--
--   C     A      G     dim1 load  dim2 load   note
--   64    256    192   0.25       0.25        low
--   128   512    96    0.50       0.50        exactly at the growth threshold
--   129   513    95    ->0.25     ->0.25      one past it: first growth each
--   192   768    64    ->0.375    ->0.375     the OLD fixed limit, exactly
--   384   1536   32    ->0.375    ->0.375     2x old useful cardinality
--   768   3072   16    ->0.375    ->0.375     4x old useful cardinality
--
-- dim1 starts at 256 slots and dim2 at 1024, unchanged, so the loads above are
-- against the real capacities rather than nominal labels.

DROP TABLE IF EXISTS reg2_dim, dim_company_d, dim_account_d;

CREATE TABLE dim_company_d (
    company_key   int4 NOT NULL,
    company_group int4 NOT NULL
);

CREATE TABLE dim_account_d (
    account_key   int8 NOT NULL,
    account_group int4 NOT NULL
);

-- Same layout as reg2_card: the five columns the report reads form a
-- fixed-width NOT NULL prefix, so the fixed-offset heap path is eligible.
CREATE TABLE reg2_dim (
    period       int4          NOT NULL,   -- 1
    company_key  int4          NOT NULL,   -- 2
    account_key  int8          NOT NULL,   -- 3
    debit_cents  int8          NOT NULL,   -- 4
    credit_cents int8          NOT NULL,   -- 5
    quantity     int8,
    debit        numeric(18,2),
    credit       numeric(18,2) NOT NULL,
    comment      text
);

-- Rebuild the fact table and both dimensions for one ladder point.
--
--   n_comp   distinct company keys      = dim1 entries
--   n_acct   distinct account keys      = dim2 entries
--   n_grp    distinct account_group values
--
-- Group cardinality is n_comp * n_grp; the caller is expected to keep that
-- constant across the ladder. Keys are sequential from 1, which the hash maps
-- collision-free -- that is the point: this ladder measures load factor and
-- growth, not collision behaviour, and skew is a later milestone.
CREATE OR REPLACE FUNCTION xpe_set_dim_cardinality(n_comp int, n_acct int, n_grp int)
RETURNS text LANGUAGE plpgsql AS $$
DECLARE rows_in bigint; groups bigint;
BEGIN
    IF n_comp < 1 OR n_acct < 1 OR n_grp < 1 OR n_grp > n_acct THEN
        RAISE EXCEPTION 'xpe_set_dim_cardinality: bad shape (%, %, %)',
              n_comp, n_acct, n_grp;
    END IF;

    TRUNCATE dim_company_d;
    INSERT INTO dim_company_d
    SELECT g + 1, (g % 8) + 1 FROM generate_series(0, n_comp - 1) g;

    TRUNCATE dim_account_d;
    INSERT INTO dim_account_d
    SELECT g + 1, (g % n_grp) + 1 FROM generate_series(0, n_acct - 1) g;

    TRUNCATE reg2_dim;
    INSERT INTO reg2_dim
    SELECT (i % 12) + 1,
           (i % n_comp) + 1,
           ((i / n_comp) % n_acct) + 1,
           (i % 997) * 100 + 1,
           (i % 991) * 70 + 3,
           CASE WHEN i % 9 = 0 THEN NULL ELSE (i % 13) END,
           CASE WHEN i % 7 = 0 THEN NULL ELSE ((i % 500) + 1)::numeric / 100 END,
           ((i % 400) + 1)::numeric / 100,
           CASE WHEN i % 5 = 0 THEN NULL ELSE 'row ' || i END
    FROM generate_series(0, 1032191) i;

    ANALYZE reg2_dim;
    ANALYZE dim_company_d;
    ANALYZE dim_account_d;

    SELECT count(*) INTO rows_in FROM reg2_dim WHERE period BETWEEN 1 AND 12;
    SELECT count(*) INTO groups FROM (
        SELECT c.company_group, a.account_group, r.company_key
        FROM reg2_dim r
        JOIN dim_company_d c ON c.company_key = r.company_key
        JOIN dim_account_d a ON a.account_key = r.account_key
        WHERE r.period BETWEEN 1 AND 12
        GROUP BY 1, 2, 3) s;

    RETURN 'rows=' || rows_in
        || ' dim1_entries=' || (SELECT count(*) FROM dim_company_d)
        || ' dim2_entries=' || (SELECT count(*) FROM dim_account_d)
        || ' groups=' || groups;
END $$;
