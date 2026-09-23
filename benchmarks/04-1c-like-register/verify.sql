-- Dataset properties, measured rather than asserted in prose.
--
-- The functional-dependency check is the one that matters: if company_key
-- determined account_key (or the other way round), the join would be trivial
-- and the group count would collapse. A generator derived from a single
-- counter does exactly that by accident.
\pset footer off

SELECT 'rows'                       AS property, count(*)::text AS value FROM reg_buh
UNION ALL SELECT 'distinct period_key',  count(DISTINCT period_key)::text  FROM reg_buh
UNION ALL SELECT 'distinct company_key', count(DISTINCT company_key)::text FROM reg_buh
UNION ALL SELECT 'distinct account_key', count(DISTINCT account_key)::text FROM reg_buh
UNION ALL SELECT 'distinct (company, account) pairs',
                 count(*)::text FROM (SELECT DISTINCT company_key, account_key FROM reg_buh) p
UNION ALL SELECT 'pairs if independent (50 x 200)', (50*200)::text
UNION ALL SELECT 'account_keys per company (min/max)',
                 min(n)::text || ' / ' || max(n)::text
          FROM (SELECT company_key, count(DISTINCT account_key) n FROM reg_buh GROUP BY 1) q
UNION ALL SELECT 'companies per account (min/max)',
                 min(n)::text || ' / ' || max(n)::text
          FROM (SELECT account_key, count(DISTINCT company_key) n FROM reg_buh GROUP BY 1) q
UNION ALL SELECT 'report groups', count(*)::text FROM (
                 SELECT p.year, a.account_group, r.company_key
                 FROM reg_buh r
                 JOIN dim_period  p ON p.period_key  = r.period_key
                 JOIN dim_account a ON a.account_key = r.account_key
                 GROUP BY 1,2,3) g
UNION ALL SELECT 'group size (min/max rows)',
                 min(n)::text || ' / ' || max(n)::text FROM (
                 SELECT count(*) n
                 FROM reg_buh r
                 JOIN dim_period  p ON p.period_key  = r.period_key
                 JOIN dim_account a ON a.account_key = r.account_key
                 GROUP BY p.year, a.account_group, r.company_key) g
UNION ALL SELECT 'amount_dt (min/max)', min(amount_dt)::text || ' / ' || max(amount_dt)::text FROM reg_buh
UNION ALL SELECT 'amount_kt (min/max)', min(amount_kt)::text || ' / ' || max(amount_kt)::text FROM reg_buh;
