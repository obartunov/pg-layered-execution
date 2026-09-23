-- Dataset 1c-like-v1. Fully synthetic; shares no table with reconstructed-v1
-- and no file with any earlier experiment.
--
-- The three tables map onto the elements of a 1C-like accounting register:
--
--   reg_buh      the register's movement records for one closed annual section
--   dim_period   the calendar: period_key -> year, month
--   dim_account  the chart of accounts: account_key -> account_group
--
-- Both dimensions are read BY ATTNUM (1 and 2) by the pipeline, and reg_buh at
-- 1,2,3,6,7; xpb_check_attnos() and xpb_dim_check_shape() refuse anything else
-- rather than reading past natts.
--
-- amount_dt / amount_kt are integers on purpose: int64 accumulation over them
-- is exact, which is what lets every path be compared by one checksum. This is
-- a property of the benchmark dataset, not numeric support.

DROP TABLE IF EXISTS reg_buh, dim_period, dim_account;

-- one closed annual section: period_key 1..12, no other year present
CREATE TABLE reg_buh (
    period_key  int NOT NULL,   -- accounting period within the year
    company_key int NOT NULL,   -- organisation
    account_key int NOT NULL,   -- account from the chart of accounts
    debit_key   int NOT NULL,   -- correspondence, carried but not aggregated
    credit_key  int NOT NULL,   -- correspondence, carried but not aggregated
    amount_dt   int NOT NULL,   -- debit turnover of the movement
    amount_kt   int NOT NULL,   -- credit turnover of the movement
    payload     int NOT NULL    -- unrelated column, kept so the row is not
                                -- narrower than the projection the report uses
);

CREATE TABLE dim_period (
    period_key int NOT NULL,
    year       int NOT NULL,
    month      int NOT NULL
);
INSERT INTO dim_period
SELECT id, 2026, id FROM generate_series(1, 12) id;

-- 200 accounts in 4 groups of 50
CREATE TABLE dim_account (
    account_key   int NOT NULL,
    account_group int NOT NULL
);
INSERT INTO dim_account
SELECT id, (id - 1) / 50 + 1 FROM generate_series(1, 200) id;
