-- Benchmark 05-E: a fact table whose group cardinality can be dialled without
-- touching a single fact row.
--
-- 05-A..05-D moved the bottleneck around inside the SOURCE. 05-E moves it
-- downstream on purpose, to the aggregation hash table, and asks how the
-- pipeline behaves as the number of live groups grows.
--
-- The group key of the v2 report is (company_group, account_group,
-- company_key). company_group is a function of company_key, so the cardinality
-- is exactly
--
--     distinct(company_key) x distinct(account_group)
--
-- and account_group is a DIMENSION payload. That is the whole trick here:
-- changing how many distinct values dim_account_c.account_group carries
-- changes the group count while leaving
--
--     the fact table byte-identical
--     the row count identical
--     the predicate and selectivity identical
--     both joins identical in shape and in hash occupancy
--
-- which is what section 3 of the brief demands -- only group cardinality may
-- vary. Rebuilding a differently-keyed fact table per point would have
-- confounded cardinality with source and join cost.
--
-- Sizing, and why these numbers:
--
--   128 companies    dim1 hash is 128/192 of its load limit (67%)
--   384 accounts     dim2 hash is 384/768 of its load limit (50%)
--
-- Both dimension hashes stay at a constant, comfortable occupancy at every
-- cardinality point, so join cost cannot drift with the ladder. Filling either
-- dimension to its own load limit would have made join time grow alongside
-- aggregate time and spoiled the measurement.
--
--   128 x 384 = 49 152 distinct (company, account) pairs
--   x 20 rows per pair = 983 040 rows   (~1M, section 4)
--
-- Every pair occurs exactly 20 times, so when k divides 384 every group holds
-- exactly the same number of rows and the distribution is uniform by
-- construction rather than by sampling (section 8: no skew here).
--
-- Reachable cardinalities are 128*k. The aggregation hash table is static at
-- V2_GRP_CAP = 16384 with a 3/4 load limit, so 12 288 groups is the last that
-- can be held; k = 96 divides 384 and lands on exactly that number.

DROP TABLE IF EXISTS reg2_card, dim_company_c, dim_account_c;

-- Same shape as dim_company / dim_account2: key first, group payload second.
-- The v2 report reads attnums 1 and 2 of each through the typed heap source.
CREATE TABLE dim_company_c (
    company_key   int4 NOT NULL,
    company_group int4 NOT NULL
);

CREATE TABLE dim_account_c (
    account_key   int8 NOT NULL,
    account_group int4 NOT NULL
);

-- Same column layout as reg2_fixed: the five columns the report reads form a
-- fixed-width NOT NULL prefix, so the fixed-offset heap path is eligible and
-- attnums 1..5 can also be handed to ZLFS unchanged. The trailing columns are
-- carried so the tuple is not artificially narrow.
CREATE TABLE reg2_card (
    period       int4          NOT NULL,   -- 1  predicate      [read]
    company_key  int4          NOT NULL,   -- 2  join 1         [read]
    account_key  int8          NOT NULL,   -- 3  join 2         [read]
    debit_cents  int8          NOT NULL,   -- 4  measured       [read]
    credit_cents int8          NOT NULL,   -- 5  measured       [read]
    quantity     int8,                     -- 6  nullable
    debit        numeric(18,2),            -- 7  nullable numeric
    credit       numeric(18,2) NOT NULL,   -- 8  numeric
    comment      text                      -- 9  varlena, NULL
);

INSERT INTO dim_company_c
SELECT g + 1, (g % 8) + 1
FROM generate_series(0, 127) g;

-- account_group is filled by xpe_set_cardinality(); this is only a placeholder
-- so the table is never left without rows.
INSERT INTO dim_account_c
SELECT g + 1, 1
FROM generate_series(0, 383) g;

-- 20 rows per (company, account) pair, walked pair-major so that period,
-- company and account are all uniform and every combination is present.
INSERT INTO reg2_card
SELECT (i % 12) + 1,                              -- period, full range 1..12
       ((i % 49152) % 128) + 1,                   -- company_key  1..128
       ((i % 49152) / 128) + 1,                   -- account_key  1..384
       (i % 997) * 100 + 1,                       -- debit_cents
       (i % 991) * 70 + 3,                        -- credit_cents
       CASE WHEN i % 9 = 0 THEN NULL ELSE (i % 13) END,
       CASE WHEN i % 7 = 0 THEN NULL ELSE ((i % 500) + 1)::numeric / 100 END,
       ((i % 400) + 1)::numeric / 100,
       CASE WHEN i % 5 = 0 THEN NULL ELSE 'row ' || i END
FROM generate_series(0, 983039) i;

ANALYZE reg2_card;
ANALYZE dim_company_c;
ANALYZE dim_account_c;

-- Set the number of distinct account_group values, and therefore the group
-- cardinality, to 128 * k. Rewrites 384 dimension rows and nothing else: the
-- fact table, the ZLFS zone over it, and both dimension hash occupancies are
-- untouched.
--
-- When k divides 384 every group holds exactly 20 * 384 / k rows. k values
-- that do not divide 384 are allowed -- they are used only to narrow the
-- capacity boundary -- and then group sizes differ by one account's worth,
-- which verify-cardinality.sql reports as min/max rather than hiding.
CREATE OR REPLACE FUNCTION xpe_set_cardinality(k int)
RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
    IF k < 1 OR k > 384 THEN
        RAISE EXCEPTION 'xpe_set_cardinality: k must be 1..384, got %', k;
    END IF;
    TRUNCATE dim_account_c;
    INSERT INTO dim_account_c
    SELECT g + 1, (g % k) + 1
    FROM generate_series(0, 383) g;
    ANALYZE dim_account_c;
    SELECT count(*) INTO n FROM (
        SELECT c.company_group, a.account_group, r.company_key
        FROM reg2_card r
        JOIN dim_company_c c ON c.company_key = r.company_key
        JOIN dim_account_c a ON a.account_key = r.account_key
        WHERE r.period BETWEEN 1 AND 12
        GROUP BY 1, 2, 3) s;
    RETURN n;
END $$;
