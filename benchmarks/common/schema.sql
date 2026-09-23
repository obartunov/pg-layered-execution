-- Canonical benchmark schema.
--
-- The fact table matches the CSV that gen_data.py writes, column for column.
--
-- The two dimensions are addressed BY ATTNUM in the readers (attnums 1 and 2 in
-- every one of them), so their first two columns are part of the contract, not
-- a presentation choice.  xpb_dim_check_shape() refuses a table that does not
-- match rather than reading past natts.
--
-- Dataset reconstructed-v1.  dim_period below is the ORIGINAL definition,
-- recovered from an older bench/00_setup.sql in a saved snapshot -- year counts
-- from 2015 and the third column is `month`.  Nothing reads that third column;
-- it is kept because the recovered schema has it.
--
-- dim_account is NOT recovered: the snapshot's version has 500 rows and the
-- v0.1.0 run recorded 200 result groups, so it is not that table.  The one
-- below is reconstructed to the documented shape.  See
-- benchmarks/HISTORICAL_RESULTS.md.

DROP TABLE IF EXISTS reg_buh, dim_period, dim_account;

CREATE TABLE reg_buh (
    period_key  int NOT NULL,
    company_key int NOT NULL,
    account_key int NOT NULL,
    debit_key   int NOT NULL,
    credit_key  int NOT NULL,
    amount_dt   int NOT NULL,
    amount_kt   int NOT NULL,
    payload     int NOT NULL
);

-- 120 periods = 10 years x 12 months, 2015..2024.
CREATE TABLE dim_period (
    period_key int NOT NULL,
    year       int NOT NULL,
    month      int NOT NULL
);
INSERT INTO dim_period
SELECT id AS period_key,
       (id - 1) / 12 + 2015 AS year,
       (id - 1) % 12 + 1    AS month
FROM generate_series(1, 120) id;

-- 200 accounts in 4 groups of 50.
CREATE TABLE dim_account (
    account_key   int NOT NULL,
    account_group int NOT NULL
);
INSERT INTO dim_account
SELECT g, (g - 1) / 50 + 1
FROM generate_series(1, 200) g;
