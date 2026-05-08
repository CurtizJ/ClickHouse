-- Eligibility-gate tests for `optimize_aggregation_in_order_limit_pushdown`.
-- For each negative case, ensure the result is correct (the optimization may
-- be silently skipped, but it must not produce wrong output).

DROP TABLE IF EXISTS t_aiolp_neg;
CREATE TABLE t_aiolp_neg (k UInt32, v UInt32) ENGINE = MergeTree ORDER BY k;
INSERT INTO t_aiolp_neg SELECT number % 100, number FROM numbers(10000);

-- 1. GROUP BY not a prefix of the sort key (here: GROUP BY v, sort key is k).
--    The pass should fall back; result must still be correct.
SELECT v, count()
FROM t_aiolp_neg GROUP BY v ORDER BY v ASC LIMIT 3
SETTINGS optimize_aggregation_in_order = 1, optimize_aggregation_in_order_limit_pushdown = 1;

-- 2. LIMIT WITH TIES.
SELECT k, count()
FROM t_aiolp_neg GROUP BY k ORDER BY k ASC LIMIT 3 WITH TIES
SETTINGS optimize_aggregation_in_order_limit_pushdown = 1;

-- 3. ORDER BY aggregate function (not a GROUP BY key prefix). Stabilize the
--    tie-break with a secondary key sort to keep the test deterministic.
SELECT k, count() AS c
FROM t_aiolp_neg GROUP BY k ORDER BY c DESC, k ASC LIMIT 3
SETTINGS optimize_aggregation_in_order = 1, optimize_aggregation_in_order_limit_pushdown = 1;

-- 4. HAVING (a FilterStep is inserted above the AggregatingStep, but the
--    rest of the plan still satisfies eligibility; correctness check).
SELECT k, count() AS c
FROM t_aiolp_neg GROUP BY k HAVING c > 50 ORDER BY k ASC LIMIT 3
SETTINGS optimize_aggregation_in_order = 1, optimize_aggregation_in_order_limit_pushdown = 1;

DROP TABLE t_aiolp_neg;
