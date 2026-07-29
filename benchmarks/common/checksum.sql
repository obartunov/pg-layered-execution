-- Correctness verification queries
-- Run each and compare MD5 against expected-checksum.txt

-- Experiment 1: aggregate only
SELECT md5(string_agg(
    period_key::text || ',' || company_key::text || ',' || total_amt::text,
    '|' ORDER BY period_key, company_key))
FROM (SELECT period_key, company_key, sum(amount_dt)::bigint AS total_amt
      FROM reg_buh WHERE period_key BETWEEN 25 AND 36
      GROUP BY period_key, company_key) r;

-- Experiment 2: two joins
SELECT md5(string_agg(
    year::text || ',' || company_key::text || ',' || total_amt::text,
    '|' ORDER BY year, company_key))
FROM (SELECT d.year, r.company_key, sum(r.amount_dt)::bigint AS total_amt
      FROM reg_buh r JOIN dim_period d ON d.period_key = r.period_key
      WHERE r.period_key BETWEEN 25 AND 36
      GROUP BY d.year, r.company_key) r;

-- Experiment 3: partition layers
SELECT md5(string_agg(
    year::text || ',' || account_group::text || ',' ||
    company_key::text || ',' || total_amt::text,
    '|' ORDER BY year, account_group, company_key))
FROM (SELECT d.year, a.account_group, r.company_key,
             sum(r.amount_dt)::bigint AS total_amt
      FROM reg_buh_layered r
      JOIN dim_period d ON d.period_key = r.period_key
      JOIN dim_account a ON a.account_key = r.account_key
      WHERE r.period_key BETWEEN 13 AND 36
      GROUP BY d.year, a.account_group, r.company_key) r;
