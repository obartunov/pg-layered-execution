-- Hash Aggregate Growth v1: a second, wider fact table for the high end of
-- the cardinality ladder.
--
-- 05-E's reg2_card is 128 companies x 384 accounts, so the largest group count
-- it can represent is 49 152. That was far past the old fixed ceiling of
-- 12 288 and more than enough for 05-E, but the growth milestone asks for
-- ~100K and beyond, which reg2_card cannot express at all.
--
-- reg2_card is deliberately NOT widened. The comparison against 05-E's
-- published aggregate timings at 6 144 / 8 192 / 12 288 groups is only
-- meaningful if the fact table and both dimension occupancies are the ones
-- 05-E measured. So the low ladder keeps running on reg2_card, unchanged, and
-- this table carries the high ladder on its own.
--
-- Sizing is set by the harness, not by taste:
--
--   192 companies    the most V2_DIM1_CAP (256, 3/4 load) can hold
--   768 accounts     the most V2_DIM2_CAP (1024, 3/4 load) can hold
--
-- 192 x 768 = 147 456 pairs is therefore the largest group count this report
-- can express at all, and the dimension hashes sit at exactly their limit to
-- reach it. That is the point: after the group table stopped being the
-- ceiling, the dimension caps became the next one, and they are the same kind
-- of compile-time constant.
--
--   192 x 768 = 147 456 pairs, x 7 rows per pair = 1 032 192 rows (~1M)
--
-- Every pair occurs exactly 7 times, so when k divides 768 every group holds
-- exactly 7 * 768 / k rows and the distribution is uniform by construction,
-- as in 05-E. No skew.

DROP TABLE IF EXISTS reg2_card2, dim_company_c2, dim_account_c2;

CREATE TABLE dim_company_c2 (
    company_key   int4 NOT NULL,
    company_group int4 NOT NULL
);

CREATE TABLE dim_account_c2 (
    account_key   int8 NOT NULL,
    account_group int4 NOT NULL
);

-- Same column layout as reg2_fixed and reg2_card: the five columns the report
-- reads are a fixed-width NOT NULL prefix, so both the fixed-offset heap path
-- and ZLFS take attnums 1..5 unchanged.
CREATE TABLE reg2_card2 (
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

INSERT INTO dim_company_c2
SELECT g + 1, (g % 8) + 1
FROM generate_series(0, 191) g;

INSERT INTO dim_account_c2
SELECT g + 1, 1
FROM generate_series(0, 767) g;

-- 7 rows per (company, account) pair, walked pair-major.
INSERT INTO reg2_card2
SELECT (i % 12) + 1,                              -- period, full range 1..12
       ((i % 147456) % 192) + 1,                  -- company_key  1..192
       ((i % 147456) / 192) + 1,                  -- account_key  1..768
       (i % 997) * 100 + 1,
       (i % 991) * 70 + 3,
       CASE WHEN i % 9 = 0 THEN NULL ELSE (i % 13) END,
       CASE WHEN i % 7 = 0 THEN NULL ELSE ((i % 500) + 1)::numeric / 100 END,
       ((i % 400) + 1)::numeric / 100,
       CASE WHEN i % 5 = 0 THEN NULL ELSE 'row ' || i END
FROM generate_series(0, 1032191) i;

ANALYZE reg2_card2;
ANALYZE dim_company_c2;
ANALYZE dim_account_c2;

-- Group cardinality becomes 192 * k, by rewriting 768 dimension rows and
-- nothing else, exactly as xpe_set_cardinality does for reg2_card.
CREATE OR REPLACE FUNCTION xpe_set_cardinality_wide(k int)
RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
    IF k < 1 OR k > 768 THEN
        RAISE EXCEPTION 'xpe_set_cardinality_wide: k must be 1..768, got %', k;
    END IF;
    TRUNCATE dim_account_c2;
    INSERT INTO dim_account_c2
    SELECT g + 1, (g % k) + 1
    FROM generate_series(0, 767) g;
    ANALYZE dim_account_c2;
    SELECT count(*) INTO n FROM (
        SELECT c.company_group, a.account_group, r.company_key
        FROM reg2_card2 r
        JOIN dim_company_c2 c ON c.company_key = r.company_key
        JOIN dim_account_c2 a ON a.account_key = r.account_key
        WHERE r.period BETWEEN 1 AND 12
        GROUP BY 1, 2, 3) s;
    RETURN n;
END $$;
