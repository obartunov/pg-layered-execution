-- Dataset 1c-like-v2. Shares no table and no file with 1c-like-v1; benchmark
-- 04's schema, data and checksum are untouched.
--
-- The Typed Batch Contract v2 row shape. Two properties matter:
--
--   * `comment` is a varlena at attnum 2, BEFORE every fixed-width column the
--     pipeline reads. That is the layout the fixed-offset heap source used to
--     read at the wrong byte offsets, so the heap arm of this benchmark runs
--     on the generic deform path -- deliberately, and recorded as such.
--
--   * rows are LOADED IN PERIOD ORDER, so pgColumnar row groups have narrow
--     period ranges and a predicate on `period` can skip whole groups.
--     1c-like-v1 was round-robin by period, which is why benchmark 04 saw no
--     pruning at all. verify.sql proves this against pgcolumnar.zone_map
--     rather than assuming it from the insert order.
--
-- Money: `debit_cents` / `credit_cents` (int8, NOT NULL) are what the report
-- aggregates, because every batch source can carry an int8. `debit` /
-- `credit` (numeric, debit nullable) are separate resources in the same row:
-- they exercise the deform path and the numeric accumulator, but no
-- pgColumnar or ZLFS source can carry a numeric today, so aggregating them
-- would leave nothing to compare across paths. Measured once on heap alone,
-- and stated in the README.

DROP TABLE IF EXISTS reg2, reg2_col, dim_company, dim_account2;

CREATE TABLE reg2 (
    period       int4          NOT NULL,   -- 1  range predicate, clustering key
    comment      text,                     -- 2  varlena, NULL, ahead of the rest
    company_key  int4          NOT NULL,   -- 3  join 1
    account_key  int8          NOT NULL,   -- 4  join 2, past 2^31
    quantity     int8,                     -- 5  nullable int8
    debit        numeric(18,2),            -- 6  nullable numeric
    credit       numeric(18,2) NOT NULL,   -- 7  numeric
    debit_cents  int8          NOT NULL,   -- 8  measured
    credit_cents int8          NOT NULL    -- 9  measured
);

-- 50 companies in 5 groups of 10
CREATE TABLE dim_company (
    company_key   int4 NOT NULL,
    company_group int4 NOT NULL
);
INSERT INTO dim_company
SELECT id, (id - 1) / 10 + 1 FROM generate_series(1, 50) id;

-- 200 accounts in 4 groups of 50, keyed past 2^31
CREATE TABLE dim_account2 (
    account_key   int8 NOT NULL,
    account_group int4 NOT NULL
);
INSERT INTO dim_account2
SELECT 4000000000 + id, (id - 1) / 50 + 1 FROM generate_series(1, 200) id;
