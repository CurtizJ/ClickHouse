-- EXPLAIN PIPELINE shape assertion for the new optimization.
--
-- When `optimize_aggregation_in_order_limit_pushdown = 1` and the eligibility
-- gate matches, the multi-stream in-order branch of `AggregatingStep::transformPipeline`
-- must:
--   * not insert `MergeSortingTransform` (the buffering full-sort that blocks
--     LIMIT backpressure when present);
--   * not perform `pipeline.resize(new_merge_threads)` after
--     `FinishAggregatingInOrderTransform` (no `Resize 1 → M`);
--   * keep a single `MergingAggregatedBucketTransform` (no `× M` fan-out).
--
-- We verify on a multi-part table where the in-order pipeline runs with
-- multiple per-part streams, since the single-stream branch of
-- `transformPipeline` is already optimal.

DROP TABLE IF EXISTS t_aiolp_pipe;
CREATE TABLE t_aiolp_pipe (k UInt32) ENGINE = MergeTree ORDER BY k
SETTINGS index_granularity = 8192;

-- Three parts force the multi-stream in-order branch.
SYSTEM STOP MERGES t_aiolp_pipe;
INSERT INTO t_aiolp_pipe SELECT number % 1000 FROM numbers(1000000);
INSERT INTO t_aiolp_pipe SELECT number % 1000 FROM numbers(1000000);
INSERT INTO t_aiolp_pipe SELECT number % 1000 FROM numbers(1000000);

-- Optimization on: assert no MergeSortingTransform (count == 0).
SELECT count()
FROM viewExplain('EXPLAIN PIPELINE', '', (
    SELECT k, count() FROM t_aiolp_pipe GROUP BY k ORDER BY k ASC LIMIT 3
    SETTINGS optimize_aggregation_in_order = 1, optimize_read_in_order = 1,
             optimize_aggregation_in_order_limit_pushdown = 1, max_threads = 4
))
WHERE explain LIKE '%MergeSortingTransform%';

-- Optimization on: assert no `Resize 1 → ` (the parallel bucket fan-out is gone).
SELECT count()
FROM viewExplain('EXPLAIN PIPELINE', '', (
    SELECT k, count() FROM t_aiolp_pipe GROUP BY k ORDER BY k ASC LIMIT 3
    SETTINGS optimize_aggregation_in_order = 1, optimize_read_in_order = 1,
             optimize_aggregation_in_order_limit_pushdown = 1, max_threads = 4
))
WHERE match(explain, 'Resize 1 → [2-9]');

-- Optimization on: only one MergingAggregatedBucketTransform (no `× N`).
SELECT count()
FROM viewExplain('EXPLAIN PIPELINE', '', (
    SELECT k, count() FROM t_aiolp_pipe GROUP BY k ORDER BY k ASC LIMIT 3
    SETTINGS optimize_aggregation_in_order = 1, optimize_read_in_order = 1,
             optimize_aggregation_in_order_limit_pushdown = 1, max_threads = 4
))
WHERE match(explain, 'MergingAggregatedBucketTransform × [2-9]');

-- Optimization off: MergeSortingTransform comes back, confirming the regression
-- this optimization is meant to fix.
SELECT count() > 0
FROM viewExplain('EXPLAIN PIPELINE', '', (
    SELECT k, count() FROM t_aiolp_pipe GROUP BY k ORDER BY k ASC LIMIT 3
    SETTINGS optimize_aggregation_in_order = 1, optimize_read_in_order = 1,
             optimize_aggregation_in_order_limit_pushdown = 0, max_threads = 4
))
WHERE explain LIKE '%MergeSortingTransform%';

DROP TABLE t_aiolp_pipe;
