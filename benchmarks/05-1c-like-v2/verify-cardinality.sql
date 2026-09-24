-- Benchmark 05-E: what a cardinality point actually is, measured rather than
-- assumed (section 7).
--
-- A case is never labelled "12K" on the strength of the k that was requested.
-- The group count is counted in SQL, from the same join and the same predicate
-- the pipeline uses, and the group-size distribution is reported so that a
-- point which is not actually uniform cannot pass as one.
--
-- Run with -v k=<k> after xpe_set_cardinality(k).

\set ON_ERROR_STOP on

SELECT 'input rows        ' || count(*) FROM reg2_card WHERE period BETWEEN 1 AND 12;
SELECT 'total rows        ' || count(*) FROM reg2_card;

-- The group key of the pipeline, spelled out: company_group is a function of
-- company_key, so cardinality is distinct(company_key) x distinct(account_group).
WITH g AS (
    SELECT c.company_group, a.account_group, r.company_key, count(*) AS n
    FROM reg2_card r
    JOIN dim_company_c  c ON c.company_key  = r.company_key
    JOIN dim_account_c  a ON a.account_key  = r.account_key
    WHERE r.period BETWEEN 1 AND 12
    GROUP BY 1, 2, 3)
SELECT 'groups            ' || count(*)                            FROM g
UNION ALL
SELECT 'rows per group    avg=' || round(avg(n), 2)
         || ' min=' || min(n) || ' max=' || max(n)
         || ' median=' || percentile_cont(0.5) WITHIN GROUP (ORDER BY n)   FROM g
UNION ALL
SELECT 'uniform           ' || CASE WHEN min(n) = max(n) THEN 'yes (every group identical)'
                                    ELSE 'no (max/min = ' || round(max(n)::numeric / min(n), 3) || ')' END
  FROM g;

-- The dimensions must stay at constant occupancy across the ladder, or join
-- cost would drift with cardinality and confound the measurement.
SELECT 'dim_company_c     rows=' || count(*) || ' distinct company_group=' || count(DISTINCT company_group)
  FROM dim_company_c
UNION ALL
SELECT 'dim_account_c     rows=' || count(*) || ' distinct account_group=' || count(DISTINCT account_group)
  FROM dim_account_c;

-- Independent check that only the dimension moved: the fact table's own key
-- distribution must be identical at every point of the ladder.
SELECT 'fact keys         distinct company_key=' || count(DISTINCT company_key)
         || ' distinct account_key=' || count(DISTINCT account_key)
         || ' distinct period=' || count(DISTINCT period)
  FROM reg2_card;
