-- Benchmark 05-B: two heap layouts holding logically identical rows.
--
-- 05-A found that a wide generic-deform heap source is slower than the
-- pgColumnar source, where benchmark 04's narrow fixed-layout heap source was
-- faster. That comparison is confounded: row width, types, NULLability,
-- attribute order, source path and generator all differ between the two.
--
-- These two tables differ in EXACTLY ONE respect: whether the five columns
-- the report reads form a fixed-width NOT NULL prefix. Same values, same row
-- order, same payload, same period clustering.
--
--   reg2_bad     the 05-A shape. `comment` (varlena) sits at attnum 2, ahead
--                of every column the report reads, so the fixed-offset path
--                cannot address the projection and the source MUST deform.
--
--   reg2_fixed   the same nine columns reordered so the five the report reads
--                come first and are all fixed-width NOT NULL. The varlena,
--                the numerics and the nullable int8 are still there, still
--                the same values -- they just sit after the projection, where
--                they cannot shift it.
--
-- Deliberately NOT narrow: the tuple keeps its full payload. The only thing
-- that changes is whether the requested prefix is fixed-layout addressable.
--
-- Attribute reordering changes padding, so the two are not guaranteed to be
-- the same size on disk. verify-layouts.sql measures that rather than
-- assuming it, and the README reports the difference.

DROP TABLE IF EXISTS reg2_bad, reg2_fixed;

-- A: the difficult layout. Projection = attnums 1, 3, 4, 8, 9.
CREATE TABLE reg2_bad (
    period       int4          NOT NULL,   -- 1  predicate      [read]
    comment      text,                     -- 2  varlena, NULL
    company_key  int4          NOT NULL,   -- 3  join 1         [read]
    account_key  int8          NOT NULL,   -- 4  join 2         [read]
    quantity     int8,                     -- 5  nullable
    debit        numeric(18,2),            -- 6  nullable numeric
    credit       numeric(18,2) NOT NULL,   -- 7  numeric
    debit_cents  int8          NOT NULL,   -- 8  measured       [read]
    credit_cents int8          NOT NULL    -- 9  measured       [read]
);

-- B: same row, projection moved to the front. Projection = attnums 1..5,
-- every one of them fixed-width and NOT NULL, so the fixed-offset path is
-- eligible. Everything after attnum 5 is unconstrained and is ignored by the
-- offset computation.
CREATE TABLE reg2_fixed (
    period       int4          NOT NULL,   -- 1  predicate      [read]
    company_key  int4          NOT NULL,   -- 2  join 1         [read]
    account_key  int8          NOT NULL,   -- 3  join 2         [read]
    debit_cents  int8          NOT NULL,   -- 4  measured       [read]
    credit_cents int8          NOT NULL,   -- 5  measured       [read]
    quantity     int8,                     -- 6  nullable
    debit        numeric(18,2),            -- 7  nullable numeric
    credit       numeric(18,2) NOT NULL,   -- 8  numeric
    comment      text                      -- 9  varlena, NULL
);
