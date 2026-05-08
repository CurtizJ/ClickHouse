-- Tags: long
-- Reproducer for https://github.com/ClickHouse/support-escalation/issues/7636
-- Verifies that GROUP BY + LIMIT on the primary key reads only enough rows to
-- produce the requested number of distinct groups, instead of scanning the full
-- table.

DROP TABLE IF EXISTS t_aiolp;

CREATE TABLE t_aiolp (k UInt64, v UInt64) ENGINE = MergeTree ORDER BY k;

-- 1M rows, 10K distinct keys, 100 rows per key. Density chosen so the prefix of
-- the table that contains LIMIT distinct keys is much smaller than the whole.
INSERT INTO t_aiolp (k, v)
SELECT number % 10000, number FROM numbers(1000000);

-- 1. Single-key GROUP BY = sort key, ORDER BY ASC. Result correctness.
SELECT k, count() AS cnt
FROM t_aiolp GROUP BY k ORDER BY k ASC LIMIT 5
SETTINGS optimize_aggregation_in_order = 1, optimize_read_in_order = 1, optimize_aggregation_in_order_limit_pushdown = 1;

-- 2. Same query with the optimization disabled — must produce identical result.
SELECT k, count() AS cnt
FROM t_aiolp GROUP BY k ORDER BY k ASC LIMIT 5
SETTINGS optimize_aggregation_in_order_limit_pushdown = 0;

-- 3. DESC variant.
SELECT k, count() AS cnt
FROM t_aiolp GROUP BY k ORDER BY k DESC LIMIT 5
SETTINGS optimize_aggregation_in_order = 1, optimize_read_in_order = 1, optimize_aggregation_in_order_limit_pushdown = 1;

-- 4. WHERE/PREWHERE present — exercises the larger Lever B row-count budget.
-- The predicate keeps the first 50 of every 100 rows for each k, regardless of
-- key parity (avoiding artifacts from filters correlated with the sort key).
SELECT k, count() AS cnt
FROM t_aiolp WHERE v < 500000 GROUP BY k ORDER BY k ASC LIMIT 5
SETTINGS optimize_aggregation_in_order = 1, optimize_read_in_order = 1, optimize_aggregation_in_order_limit_pushdown = 1;

-- 5. Composite GROUP BY where ORDER BY is a strict prefix.
DROP TABLE IF EXISTS t_aiolp_composite;
CREATE TABLE t_aiolp_composite (k1 UInt32, k2 UInt32, v UInt64)
ENGINE = MergeTree ORDER BY (k1, k2);

INSERT INTO t_aiolp_composite SELECT number % 1000, number % 100, number FROM numbers(100000);

SELECT k1, k2, count()
FROM t_aiolp_composite GROUP BY k1, k2 ORDER BY k1, k2 ASC LIMIT 5
SETTINGS optimize_aggregation_in_order = 1, optimize_read_in_order = 1, optimize_aggregation_in_order_limit_pushdown = 1;

DROP TABLE t_aiolp_composite;

DROP TABLE t_aiolp;
