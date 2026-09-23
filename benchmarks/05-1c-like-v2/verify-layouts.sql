-- Benchmark 05-B: prove the two heap layouts hold logically identical rows,
-- and record how much their PHYSICAL size differs.
--
-- Identical logical data does not imply equal width: reordering attributes
-- changes alignment padding. If reg2_fixed turns out materially smaller than
-- reg2_bad, then the A-versus-B difference is "layout + size", not layout
-- alone, and the README has to say so. Measured here rather than assumed.

\pset footer off

\echo '--- row counts and logical equivalence ---'

SELECT 'reg2_bad   rows ' || lpad(count(*)::text, 10) FROM reg2_bad
UNION ALL
SELECT 'reg2_fixed rows ' || lpad(count(*)::text, 10) FROM reg2_fixed;

-- Every column of every row, ordered canonically, hashed. This catches a
-- generator or load mistake before any timing is taken.
SELECT CASE WHEN b.ck = f.ck
            THEN 'logical checksum IDENTICAL: ' || b.ck
            ELSE 'MISMATCH: bad ' || b.ck || '  fixed ' || f.ck END
FROM (SELECT md5(string_agg(period||'|'||company_key||'|'||account_key||'|'
                            ||coalesce(quantity::text,'\N')||'|'
                            ||coalesce(debit::text,'\N')||'|'||credit||'|'
                            ||debit_cents||'|'||credit_cents||'|'
                            ||coalesce(comment,'\N'),
                            E'\n' ORDER BY period, company_key, account_key,
                                           debit_cents, credit_cents,
                                           coalesce(comment,'\N'))) ck
      FROM reg2_bad) b,
     (SELECT md5(string_agg(period||'|'||company_key||'|'||account_key||'|'
                            ||coalesce(quantity::text,'\N')||'|'
                            ||coalesce(debit::text,'\N')||'|'||credit||'|'
                            ||debit_cents||'|'||credit_cents||'|'
                            ||coalesce(comment,'\N'),
                            E'\n' ORDER BY period, company_key, account_key,
                                           debit_cents, credit_cents,
                                           coalesce(comment,'\N'))) ck
      FROM reg2_fixed) f;

\echo ''
\echo '--- distributions must match ---'

SELECT 'period distribution   ' ||
       CASE WHEN (SELECT count(*) FROM (
                    SELECT period, count(*) c FROM reg2_bad GROUP BY period
                    EXCEPT
                    SELECT period, count(*) c FROM reg2_fixed GROUP BY period) d) = 0
            THEN 'identical' ELSE 'DIFFERS' END
UNION ALL
SELECT 'company distribution  ' ||
       CASE WHEN (SELECT count(*) FROM (
                    SELECT company_key, count(*) c FROM reg2_bad GROUP BY company_key
                    EXCEPT
                    SELECT company_key, count(*) c FROM reg2_fixed GROUP BY company_key) d) = 0
            THEN 'identical' ELSE 'DIFFERS' END
UNION ALL
SELECT 'account distribution  ' ||
       CASE WHEN (SELECT count(*) FROM (
                    SELECT account_key, count(*) c FROM reg2_bad GROUP BY account_key
                    EXCEPT
                    SELECT account_key, count(*) c FROM reg2_fixed GROUP BY account_key) d) = 0
            THEN 'identical' ELSE 'DIFFERS' END
UNION ALL
SELECT 'payload length distr  ' ||
       CASE WHEN (SELECT count(*) FROM (
                    SELECT length(comment) l, count(*) c FROM reg2_bad GROUP BY 1
                    EXCEPT
                    SELECT length(comment) l, count(*) c FROM reg2_fixed GROUP BY 1) d) = 0
            THEN 'identical' ELSE 'DIFFERS' END
UNION ALL
SELECT 'NULL counts           ' ||
       CASE WHEN (SELECT count(*) FROM (
                    SELECT count(*) FILTER (WHERE comment IS NULL),
                           count(*) FILTER (WHERE quantity IS NULL),
                           count(*) FILTER (WHERE debit IS NULL) FROM reg2_bad
                    EXCEPT
                    SELECT count(*) FILTER (WHERE comment IS NULL),
                           count(*) FILTER (WHERE quantity IS NULL),
                           count(*) FILTER (WHERE debit IS NULL) FROM reg2_fixed) d) = 0
            THEN 'identical' ELSE 'DIFFERS' END;

\echo ''
\echo '--- PHYSICAL size: reordering changes padding, so this is measured ---'

SELECT rpad(c.relname, 12)
       || ' relation ' || lpad(pg_size_pretty(pg_relation_size(c.oid)), 9)
       || '  total ' || lpad(pg_size_pretty(pg_total_relation_size(c.oid)), 9)
       || '  pages ' || lpad(c.relpages::text, 7)
       || '  avg_width ' || lpad(coalesce((SELECT sum(s.avg_width)
                                           FROM pg_stats s
                                           WHERE s.tablename = c.relname), 0)::text, 5)
FROM pg_class c
WHERE c.relname IN ('reg2_bad', 'reg2_fixed')
ORDER BY c.relname;

SELECT 'size difference (fixed - bad): '
       || (pg_relation_size('reg2_fixed'::regclass)
           - pg_relation_size('reg2_bad'::regclass)) || ' bytes, '
       || round(100.0 * (pg_relation_size('reg2_fixed'::regclass)
                         - pg_relation_size('reg2_bad'::regclass))
                / pg_relation_size('reg2_bad'::regclass), 2) || '%';

\echo ''
\echo '--- the same SQL report over both tables must agree, before xp_batch ---'

WITH b AS (
  SELECT c.company_group, a.account_group, r.company_key,
         sum(r.debit_cents) d, sum(r.credit_cents) k,
         sum(r.debit_cents - r.credit_cents) n
  FROM reg2_bad r
  JOIN dim_company c ON c.company_key = r.company_key
  JOIN dim_account2 a ON a.account_key = r.account_key
  GROUP BY 1,2,3),
f AS (
  SELECT c.company_group, a.account_group, r.company_key,
         sum(r.debit_cents) d, sum(r.credit_cents) k,
         sum(r.debit_cents - r.credit_cents) n
  FROM reg2_fixed r
  JOIN dim_company c ON c.company_key = r.company_key
  JOIN dim_account2 a ON a.account_key = r.account_key
  GROUP BY 1,2,3)
SELECT CASE WHEN (SELECT count(*) FROM (TABLE b EXCEPT TABLE f) x) = 0
             AND (SELECT count(*) FROM (TABLE f EXCEPT TABLE b) x) = 0
            THEN 'SQL report identical over both layouts ('
                 || (SELECT count(*) FROM b) || ' groups)'
            ELSE 'SQL REPORT DIFFERS between layouts' END;
