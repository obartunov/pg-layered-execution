-- pgColumnar row-group layout for reg2_col.
--
-- Benchmark 05-A is meaningless unless a predicate on `period` can skip whole
-- row groups. This reports the physical organisation from pgColumnar's own
-- catalogs -- the same zone maps its planner prunes on -- rather than
-- inferring it from the insert order.
--
-- period is attnum 1, so column_index 0 (zone_map.column_index is 0-based).
-- vector_index = -1 is the whole-chunk aggregate, which is the granularity
-- row-group elimination works at.
--
-- zone_map.minimum/maximum are bytea "encoded per the column type". For int4
-- that is the 4-byte little-endian value, decoded here with get_byte().

\pset footer off

\echo '--- row groups, with the period range each one covers ---'

WITH zm AS (
    SELECT z.group_number,
           (get_byte(z.minimum,0) | (get_byte(z.minimum,1) << 8) |
            (get_byte(z.minimum,2) << 16) | (get_byte(z.minimum,3) << 24)) AS p_min,
           (get_byte(z.maximum,0) | (get_byte(z.maximum,1) << 8) |
            (get_byte(z.maximum,2) << 16) | (get_byte(z.maximum,3) << 24)) AS p_max,
           z.null_count
    FROM pgcolumnar.zone_map z
    WHERE z.storage_id = pgcolumnar.get_storage_id('reg2_col')
      AND z.column_index = 0
      AND z.vector_index = -1
)
SELECT 'rowgroup ' || lpad(s.stripeid::text, 2)
       || '  rows=' || lpad(s.rowcount::text, 8)
       || '  periods ' || lpad(zm.p_min::text, 2) || '..' || rpad(zm.p_max::text, 2)
       || '  span=' || (zm.p_max - zm.p_min + 1)
       || '  nulls=' || zm.null_count
       || '  chunks=' || s.chunkcount
       AS layout
FROM pgcolumnar.stats('reg2_col') s
JOIN zm ON zm.group_number = s.stripeid
ORDER BY s.stripeid;

\echo ''
\echo '--- pruning opportunity: row groups a period predicate must read ---'

WITH zm AS (
    SELECT z.group_number,
           (get_byte(z.minimum,0) | (get_byte(z.minimum,1) << 8) |
            (get_byte(z.minimum,2) << 16) | (get_byte(z.minimum,3) << 24)) AS p_min,
           (get_byte(z.maximum,0) | (get_byte(z.maximum,1) << 8) |
            (get_byte(z.maximum,2) << 16) | (get_byte(z.maximum,3) << 24)) AS p_max
    FROM pgcolumnar.zone_map z
    WHERE z.storage_id = pgcolumnar.get_storage_id('reg2_col')
      AND z.column_index = 0 AND z.vector_index = -1
), tot AS (SELECT count(*) AS n FROM zm)
SELECT 'periods ' || rpad(q.lo || '..' || q.hi, 6)
       || '  rowgroups read ' || lpad(count(*) FILTER (WHERE zm.p_max >= q.lo
                                                         AND zm.p_min <= q.hi)::text, 2)
       || ' of ' || tot.n
       || '   skipped ' || lpad((tot.n - count(*) FILTER (WHERE zm.p_max >= q.lo
                                                            AND zm.p_min <= q.hi))::text, 2)
       AS pruning
FROM (VALUES (1,1),(1,3),(1,6),(1,12)) AS q(lo,hi), zm, tot
GROUP BY q.lo, q.hi, tot.n
ORDER BY q.hi;

\echo ''
\echo '--- STOP CONDITION: every row group holding all 12 periods means no pruning ---'

WITH zm AS (
    SELECT (get_byte(z.minimum,0) | (get_byte(z.minimum,1) << 8) |
            (get_byte(z.minimum,2) << 16) | (get_byte(z.minimum,3) << 24)) AS p_min,
           (get_byte(z.maximum,0) | (get_byte(z.maximum,1) << 8) |
            (get_byte(z.maximum,2) << 16) | (get_byte(z.maximum,3) << 24)) AS p_max
    FROM pgcolumnar.zone_map z
    WHERE z.storage_id = pgcolumnar.get_storage_id('reg2_col')
      AND z.column_index = 0 AND z.vector_index = -1
)
SELECT CASE
         WHEN count(*) FILTER (WHERE p_min = 1 AND p_max = 12) = count(*)
           THEN 'STOP: every row group spans all 12 periods -- no pruning possible'
         ELSE 'OK: ' || count(*) FILTER (WHERE p_max - p_min + 1 <= 2)
              || ' of ' || count(*) || ' row groups span 2 periods or fewer'
       END AS verdict
FROM zm;
