-- The columnar arm of the comparison: the same rows as reg_buh, stored USING
-- pgcolumnar. Two tables rather than one, because a table has exactly one
-- storage layer and the comparison is between layers.
--
-- Optional: load.sh applies this only when the pgcolumnar extension is
-- installable, and the benchmark skips the two columnar paths when the table
-- is absent.
--
-- pgcolumnar must precede xp_batch in shared_preload_libraries: xp_batch links
-- against its reader symbols, and PostgreSQL resolves module symbols at load
-- time, so the other order fails to start the server.

CREATE EXTENSION IF NOT EXISTS pgcolumnar;

DROP TABLE IF EXISTS reg_buh_col;

CREATE TABLE reg_buh_col (
    period_key  int NOT NULL,
    company_key int NOT NULL,
    account_key int NOT NULL,
    debit_key   int NOT NULL,
    credit_key  int NOT NULL,
    amount_dt   int NOT NULL,
    amount_kt   int NOT NULL,
    payload     int NOT NULL
) USING pgcolumnar;

INSERT INTO reg_buh_col SELECT * FROM reg_buh;
