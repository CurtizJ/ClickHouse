-- The dynamic top-K filter of `ORDER BY <column> LIMIT n` is pushed down below joins and unions:
-- into the reads of the left input of a join and of every input of a union, where it is the first
-- PREWHERE read step. Where no read can take it, it is not placed at all.

DROP TABLE IF EXISTS t_top_k_left;
DROP TABLE IF EXISTS t_top_k_left_2;
DROP TABLE IF EXISTS t_top_k_right;

CREATE TABLE t_top_k_left (key UInt64, grp UInt64, value UInt64, s String)
ENGINE = MergeTree ORDER BY key
SETTINGS index_granularity = 1024;

CREATE TABLE t_top_k_left_2 AS t_top_k_left;

CREATE TABLE t_top_k_right (grp UInt64, name String)
ENGINE = MergeTree ORDER BY grp;

-- `value` is unique, so the top-K has no ties and the results are deterministic.
INSERT INTO t_top_k_left SELECT number, number % 1000, 1000000 - number, toString(number) FROM numbers(100000);
INSERT INTO t_top_k_left_2 SELECT number, number % 1000, 2000000 - number * 2, toString(number) FROM numbers(100000);
-- Only a few groups match, so most of the left rows produce no row of an inner join.
INSERT INTO t_top_k_right SELECT number * 100 + 7, 'g' || toString(number) FROM numbers(10);

SET use_top_k_dynamic_filtering = 1, use_skip_indexes_for_top_k = 0, use_query_condition_cache = 0;
SET enable_parallel_replicas = 0, query_plan_optimize_lazy_materialization = 0;
SET query_plan_max_limit_for_top_k_optimization = 1000;
SET explain_query_plan_default = 'legacy';

SELECT '-- INNER JOIN: the read of the left input takes the filter';
SELECT countIf(explain LIKE '%TopK filter column: \_\_topKFilter(value)%')
FROM (EXPLAIN actions = 1 SELECT l.key, l.value, r.name FROM t_top_k_left AS l INNER JOIN t_top_k_right AS r ON l.grp = r.grp ORDER BY l.value DESC LIMIT 5);

SELECT l.key, l.value, r.name FROM t_top_k_left AS l INNER JOIN t_top_k_right AS r ON l.grp = r.grp ORDER BY l.value DESC LIMIT 5;
SELECT l.key, l.value, r.name FROM t_top_k_left AS l INNER JOIN t_top_k_right AS r ON l.grp = r.grp ORDER BY l.value DESC LIMIT 5
SETTINGS use_top_k_dynamic_filtering = 0;

SELECT '-- LEFT JOIN: the read of the left input takes the filter';
SELECT countIf(explain LIKE '%TopK filter column: \_\_topKFilter(value)%')
FROM (EXPLAIN actions = 1 SELECT l.key, l.value, r.name FROM t_top_k_left AS l LEFT JOIN t_top_k_right AS r ON l.grp = r.grp ORDER BY l.value ASC LIMIT 5
    SETTINGS query_plan_top_k_through_join = 0);

SELECT l.key, l.value, r.name FROM t_top_k_left AS l LEFT JOIN t_top_k_right AS r ON l.grp = r.grp ORDER BY l.value ASC LIMIT 5
SETTINGS query_plan_top_k_through_join = 0;
SELECT l.key, l.value, r.name FROM t_top_k_left AS l LEFT JOIN t_top_k_right AS r ON l.grp = r.grp ORDER BY l.value ASC LIMIT 5
SETTINGS query_plan_top_k_through_join = 1;
SELECT l.key, l.value, r.name FROM t_top_k_left AS l LEFT JOIN t_top_k_right AS r ON l.grp = r.grp ORDER BY l.value ASC LIMIT 5
SETTINGS use_top_k_dynamic_filtering = 0;

SELECT '-- RIGHT and FULL JOIN: unmatched right rows carry default left columns, no filter';
SELECT countIf(explain LIKE '%topKFilter%')
FROM (EXPLAIN actions = 1 SELECT l.key, l.value, r.name FROM t_top_k_left AS l RIGHT JOIN t_top_k_right AS r ON l.grp = r.grp ORDER BY l.value DESC LIMIT 5);
SELECT countIf(explain LIKE '%topKFilter%')
FROM (EXPLAIN actions = 1 SELECT l.key, l.value, r.name FROM t_top_k_left AS l FULL JOIN t_top_k_right AS r ON l.grp = r.grp ORDER BY l.value DESC LIMIT 5);

SELECT '-- the sort column from the right input: no filter';
SELECT countIf(explain LIKE '%topKFilter%')
FROM (EXPLAIN actions = 1 SELECT l.key, r.name FROM t_top_k_left AS l INNER JOIN t_top_k_right AS r ON l.grp = r.grp ORDER BY r.name DESC LIMIT 5
    SETTINGS use_top_k_dynamic_filtering_for_variable_length_types = 1);

SELECT '-- the left input has no read to take the filter: no filter';
SELECT countIf(explain LIKE '%topKFilter%')
FROM (EXPLAIN actions = 1
    SELECT g.grp, g.m, r.name FROM (SELECT grp, max(value) AS m FROM t_top_k_left GROUP BY grp) AS g
    INNER JOIN t_top_k_right AS r ON g.grp = r.grp ORDER BY g.m DESC LIMIT 3);

SELECT g.grp, g.m, r.name FROM (SELECT grp, max(value) AS m FROM t_top_k_left GROUP BY grp) AS g
INNER JOIN t_top_k_right AS r ON g.grp = r.grp ORDER BY g.m DESC LIMIT 3;
SELECT g.grp, g.m, r.name FROM (SELECT grp, max(value) AS m FROM t_top_k_left GROUP BY grp) AS g
INNER JOIN t_top_k_right AS r ON g.grp = r.grp ORDER BY g.m DESC LIMIT 3
SETTINGS use_top_k_dynamic_filtering = 0;

SELECT '-- UNION ALL: the reads of both inputs take the filter';
SELECT countIf(explain LIKE '%TopK filter column: \_\_topKFilter(value)%')
FROM (EXPLAIN actions = 1
    SELECT key, value FROM (SELECT key, value FROM t_top_k_left UNION ALL SELECT key, value FROM t_top_k_left_2) ORDER BY value DESC LIMIT 5);

SELECT key, value FROM (SELECT key, value FROM t_top_k_left UNION ALL SELECT key, value FROM t_top_k_left_2) ORDER BY value DESC LIMIT 5;
SELECT key, value FROM (SELECT key, value FROM t_top_k_left UNION ALL SELECT key, value FROM t_top_k_left_2) ORDER BY value DESC LIMIT 5
SETTINGS use_top_k_dynamic_filtering = 0;

SELECT '-- a condition which depends on the block stops the filter: no filter';
SELECT countIf(explain LIKE '%topKFilter%')
FROM (EXPLAIN actions = 1 SELECT l.key, l.value, r.name FROM (SELECT * FROM t_top_k_left WHERE blockSize() > 0) AS l
    INNER JOIN t_top_k_right AS r ON l.grp = r.grp ORDER BY l.value DESC LIMIT 5);

SELECT '-- LEFT JOIN with a minmax index: the Sort + Limit of topKThroughJoin selects the top-K granules';
DROP TABLE IF EXISTS t_top_k_idx;
CREATE TABLE t_top_k_idx (key UInt64, grp UInt64, value UInt64, INDEX idx_value value TYPE minmax GRANULARITY 1)
ENGINE = MergeTree ORDER BY key
SETTINGS index_granularity = 1024;
INSERT INTO t_top_k_idx SELECT number, number % 1000, 1000000 - number FROM numbers(100000);

SELECT countIf(explain LIKE '%Filter TopK Granules%') > 0
FROM (EXPLAIN indexes = 1 SELECT l.key, l.value, r.name FROM t_top_k_idx AS l LEFT JOIN t_top_k_right AS r ON l.grp = r.grp ORDER BY l.value DESC LIMIT 5
    SETTINGS query_plan_top_k_through_join = 1, use_skip_indexes_for_top_k = 1);

SELECT l.key, l.value, r.name FROM t_top_k_idx AS l LEFT JOIN t_top_k_right AS r ON l.grp = r.grp ORDER BY l.value DESC LIMIT 5
SETTINGS query_plan_top_k_through_join = 1, use_skip_indexes_for_top_k = 1;
SELECT l.key, l.value, r.name FROM t_top_k_idx AS l LEFT JOIN t_top_k_right AS r ON l.grp = r.grp ORDER BY l.value DESC LIMIT 5
SETTINGS query_plan_top_k_through_join = 0, use_skip_indexes_for_top_k = 0, use_top_k_dynamic_filtering = 0;

DROP TABLE t_top_k_idx;

SELECT '-- the filter reads fewer rows of the left input';
SELECT l.key FROM t_top_k_left AS l INNER JOIN t_top_k_right AS r ON l.grp = r.grp ORDER BY l.value DESC LIMIT 5
FORMAT Null SETTINGS max_threads = 1, max_block_size = 1024, enable_join_runtime_filters = 0,
    query_plan_join_swap_table = 0, query_plan_optimize_join_order_randomize = 0, log_comment = '05256_join_on';
SELECT l.key FROM t_top_k_left AS l INNER JOIN t_top_k_right AS r ON l.grp = r.grp ORDER BY l.value DESC LIMIT 5
FORMAT Null SETTINGS max_threads = 1, max_block_size = 1024, enable_join_runtime_filters = 0,
    query_plan_join_swap_table = 0, query_plan_optimize_join_order_randomize = 0, log_comment = '05256_join_off', use_top_k_dynamic_filtering = 0;

SYSTEM FLUSH LOGS query_log;

-- `value` decreases with `key`, so the threshold is set after the first block and the top-K filter,
-- the first PREWHERE read step, rejects the rest of the left input: the main reader skips those rows.
-- A join runtime filter in the PREWHERE would keep the reader filling its first block for most of the
-- table, so the threshold would come too late to skip anything. A swapped join would read the table
-- as its build side, before the first row reaches the sorting step.
WITH
    (SELECT ProfileEvents['RowsReadByMainReader'] FROM system.query_log
        WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment = '05256_join_on' ORDER BY event_time_microseconds DESC LIMIT 1) AS rows_on,
    (SELECT ProfileEvents['RowsReadByMainReader'] FROM system.query_log
        WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment = '05256_join_off' ORDER BY event_time_microseconds DESC LIMIT 1) AS rows_off
SELECT rows_on < rows_off;

DROP TABLE t_top_k_left;
DROP TABLE t_top_k_left_2;
DROP TABLE t_top_k_right;
