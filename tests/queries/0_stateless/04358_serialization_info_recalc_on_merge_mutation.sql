-- Regression test: with `serialization_info_version` < `without_data` (i.e. `basic`/`with_types`),
-- `serialization.json` stores per-column `num_defaults`. Merges and mutations must recompute those
-- counts over the written data. The refactoring in PR #85145 dropped that recomputation, so any merge
-- or mutation of a table with a sparse-capable column threw
-- `LOGICAL_ERROR: Missed statistics for column ...`. This test exercises merge, full mutation,
-- vertical (single-column) mutation, lightweight delete and a row-reducing merge for both legacy
-- serialization versions.

DROP TABLE IF EXISTS t_ser_wide;
DROP TABLE IF EXISTS t_ser_compact;
DROP TABLE IF EXISTS t_ser_replacing;

-- Wide parts => single-column ALTER UPDATE goes through the vertical (column-only) mutation path.
CREATE TABLE t_ser_wide (id UInt64, s String, n UInt64)
ENGINE = MergeTree ORDER BY id
SETTINGS serialization_info_version = 'basic',
         ratio_of_defaults_for_sparse_serialization = 0.5,
         min_bytes_for_wide_part = 0;

-- Mostly-default data => sparse serialization is chosen, so the columns appear in serialization.json.
INSERT INTO t_ser_wide SELECT number, '', 0 FROM numbers(1000);
INSERT INTO t_ser_wide SELECT number, if(number % 20 = 0, 'x', ''), if(number % 20 = 0, number, 0) FROM numbers(1000, 1000);

-- Merge.
OPTIMIZE TABLE t_ser_wide FINAL;
SELECT 'wide after merge', count(), countIf(s != ''), sum(n) FROM t_ser_wide;

-- Vertical (single-column) mutation.
ALTER TABLE t_ser_wide UPDATE s = 'y' WHERE id = 40 SETTINGS mutations_sync = 2;
SELECT 'wide after vertical mutation', count(), countIf(s = 'y') FROM t_ser_wide;

-- Lightweight delete (row-reducing mutation) followed by a merge.
DELETE FROM t_ser_wide WHERE id >= 1980;
OPTIMIZE TABLE t_ser_wide FINAL;
SELECT 'wide after delete + merge', count() FROM t_ser_wide;

-- Compact parts => mutations rewrite all columns (full-rewrite mutation path). Use `with_types`.
CREATE TABLE t_ser_compact (id UInt64, s String, n UInt64)
ENGINE = MergeTree ORDER BY id
SETTINGS serialization_info_version = 'with_types',
         ratio_of_defaults_for_sparse_serialization = 0.5,
         min_bytes_for_wide_part = '1G';

INSERT INTO t_ser_compact SELECT number, '', 0 FROM numbers(1000);
INSERT INTO t_ser_compact SELECT number, if(number % 20 = 0, 'x', ''), if(number % 20 = 0, number, 0) FROM numbers(1000, 1000);

OPTIMIZE TABLE t_ser_compact FINAL;
SELECT 'compact after merge', count(), countIf(s != ''), sum(n) FROM t_ser_compact;

ALTER TABLE t_ser_compact UPDATE n = n + 1 WHERE id % 100 = 0 SETTINGS mutations_sync = 2;
SELECT 'compact after mutation', count(), sum(n) FROM t_ser_compact;

-- Row-reducing merge via ReplacingMergeTree: the merged part has fewer rows than the sum of inputs,
-- so the default counts genuinely have to be recomputed over the output (not summed from sources).
CREATE TABLE t_ser_replacing (id UInt64, s String, n UInt64)
ENGINE = ReplacingMergeTree ORDER BY id
SETTINGS serialization_info_version = 'basic',
         ratio_of_defaults_for_sparse_serialization = 0.5,
         min_bytes_for_wide_part = 0;

INSERT INTO t_ser_replacing SELECT number, '', 0 FROM numbers(1000);
INSERT INTO t_ser_replacing SELECT number, '', 0 FROM numbers(1000);
INSERT INTO t_ser_replacing SELECT number, if(number % 20 = 0, 'x', ''), if(number % 20 = 0, number, 0) FROM numbers(1000);

OPTIMIZE TABLE t_ser_replacing FINAL;
SELECT 'replacing after merge', count() FROM t_ser_replacing;

-- Force the vertical merge algorithm so the gathered columns go through the per-column
-- BuildStatisticsTransform path that recomputes their serialization default counts.
CREATE TABLE t_ser_vertical (id UInt64, s String, n UInt64)
ENGINE = MergeTree ORDER BY id
SETTINGS serialization_info_version = 'basic',
         ratio_of_defaults_for_sparse_serialization = 0.5,
         min_bytes_for_wide_part = 0,
         min_rows_for_wide_part = 0,
         vertical_merge_algorithm_min_rows_to_activate = 1,
         vertical_merge_algorithm_min_columns_to_activate = 1;

INSERT INTO t_ser_vertical SELECT number, '', 0 FROM numbers(1000);
INSERT INTO t_ser_vertical SELECT number, if(number % 20 = 0, 'x', ''), if(number % 20 = 0, number, 0) FROM numbers(1000, 1000);

OPTIMIZE TABLE t_ser_vertical FINAL;
SELECT 'vertical after merge', count(), countIf(s != ''), sum(n) FROM t_ser_vertical;

-- Row-reducing vertical merge: counts must be recomputed over the reduced output.
DELETE FROM t_ser_vertical WHERE id >= 1990;
OPTIMIZE TABLE t_ser_vertical FINAL;
SELECT 'vertical after delete + merge', count() FROM t_ser_vertical;

DROP TABLE t_ser_wide;
DROP TABLE t_ser_compact;
DROP TABLE t_ser_replacing;
DROP TABLE t_ser_vertical;
