-- Structural properties of dataset 1c-like-v2, MEASURED rather than asserted.
--
-- The pair count is the check that matters, and it is here because 1c-like-v1
-- got it wrong: its first generator advanced company_key and account_key with
-- co-prime strides, which looks independent and is not -- the pair cycled with
-- lcm(50,200) = 200, so 200 of the 10 000 pairs ever occurred and every
-- account belonged to exactly one company, making the second join trivial.
--
-- Physical clustering is verified separately, in inspect-rowgroups.sql,
-- against pgColumnar's own zone maps.

\pset footer off

SELECT 'rows                            ' || lpad(count(*)::text, 10) FROM reg2
UNION ALL SELECT 'distinct period                 ' || lpad(count(DISTINCT period)::text, 10) FROM reg2
UNION ALL SELECT 'distinct company_key            ' || lpad(count(DISTINCT company_key)::text, 10) FROM reg2
UNION ALL SELECT 'distinct account_key            ' || lpad(count(DISTINCT account_key)::text, 10) FROM reg2
UNION ALL SELECT 'distinct (company, account)     ' || lpad(count(*)::text, 10)
          FROM (SELECT DISTINCT company_key, account_key FROM reg2) s
UNION ALL SELECT '  ... expected 50 x 200 =       ' || lpad((50*200)::text, 10)
UNION ALL SELECT 'rows per period (min/max)       ' || lpad(min(n)::text, 10) || ' /' || lpad(max(n)::text, 9)
          FROM (SELECT count(*) n FROM reg2 GROUP BY period) s
UNION ALL SELECT 'accounts per company (min/max)  ' || lpad(min(n)::text, 10) || ' /' || lpad(max(n)::text, 9)
          FROM (SELECT count(DISTINCT account_key) n FROM reg2 GROUP BY company_key) s
UNION ALL SELECT 'companies per account (min/max) ' || lpad(min(n)::text, 10) || ' /' || lpad(max(n)::text, 9)
          FROM (SELECT count(DISTINCT company_key) n FROM reg2 GROUP BY account_key) s;

\echo ''
\echo '--- report groups, on the full annual section ---'
SELECT 'report groups                   ' || lpad(count(*)::text, 10)
FROM (SELECT c.company_group, a.account_group, r.company_key
      FROM reg2 r
      JOIN dim_company  c ON c.company_key = r.company_key
      JOIN dim_account2 a ON a.account_key = r.account_key
      GROUP BY 1,2,3) s;

\echo ''
\echo '--- NULL rates, recorded because the row shape is the point ---'
SELECT 'comment  NULL                   ' || lpad(count(*) FILTER (WHERE comment IS NULL)::text, 10)
       || '  (' || round(100.0 * count(*) FILTER (WHERE comment IS NULL) / count(*), 1) || '%)' FROM reg2
UNION ALL
SELECT 'comment  empty string           ' || lpad(count(*) FILTER (WHERE comment = '')::text, 10)
       || '  (' || round(100.0 * count(*) FILTER (WHERE comment = '') / count(*), 1) || '%)' FROM reg2
UNION ALL
SELECT 'quantity NULL                   ' || lpad(count(*) FILTER (WHERE quantity IS NULL)::text, 10)
       || '  (' || round(100.0 * count(*) FILTER (WHERE quantity IS NULL) / count(*), 1) || '%)' FROM reg2
UNION ALL
SELECT 'debit    NULL (numeric)         ' || lpad(count(*) FILTER (WHERE debit IS NULL)::text, 10)
       || '  (' || round(100.0 * count(*) FILTER (WHERE debit IS NULL) / count(*), 1) || '%)' FROM reg2
UNION ALL
SELECT 'credit   NULL (numeric)         ' || lpad(count(*) FILTER (WHERE credit IS NULL)::text, 10)
       || '  (NOT NULL by schema)' FROM reg2;

\echo ''
\echo '--- measured columns: the report aggregates these, and they are NOT NULL ---'
SELECT 'account_key min/max             ' || min(account_key) || ' / ' || max(account_key)
       || '   (past 2^31: ' || (min(account_key) > 2147483647) || ')' FROM reg2
UNION ALL
SELECT 'debit_cents  min/max            ' || min(debit_cents) || ' / ' || max(debit_cents) FROM reg2
UNION ALL
SELECT 'credit_cents min/max            ' || min(credit_cents) || ' / ' || max(credit_cents) FROM reg2
UNION ALL
SELECT 'sum(debit_cents)                ' || sum(debit_cents)
       || '   (past 2^31: ' || (sum(debit_cents) > 2147483647) || ')' FROM reg2;

\echo ''
\echo '--- heap and columnar hold identical rows ---'
SELECT CASE WHEN h.ck = c.ck THEN 'heap and pgColumnar agree: ' || h.ck
            ELSE 'MISMATCH: heap ' || h.ck || ' vs columnar ' || c.ck END
FROM (SELECT md5(string_agg(period||','||coalesce(comment,'\N')||','||company_key||','
                            ||account_key||','||coalesce(quantity::text,'\N')||','
                            ||coalesce(debit::text,'\N')||','||credit||','
                            ||debit_cents||','||credit_cents,
                            '|' ORDER BY period, company_key, account_key,
                                         debit_cents, credit_cents)) ck FROM reg2) h,
     (SELECT md5(string_agg(period||','||coalesce(comment,'\N')||','||company_key||','
                            ||account_key||','||coalesce(quantity::text,'\N')||','
                            ||coalesce(debit::text,'\N')||','||credit||','
                            ||debit_cents||','||credit_cents,
                            '|' ORDER BY period, company_key, account_key,
                                         debit_cents, credit_cents)) ck FROM reg2_col) c;
