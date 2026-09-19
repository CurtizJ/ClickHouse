-- The dynamic top-K filter of `ORDER BY <column> LIMIT n` is the first PREWHERE read step, and the
-- conditions of the query are moved to PREWHERE as usual: they are evaluated after the top-K filter,
-- only for the rows that can still reach the result.

DROP TABLE IF EXISTS t_top_k_first;

CREATE TABLE t_top_k_first (key UInt64, value UInt64, s String)
ENGINE = MergeTree ORDER BY key
SETTINGS index_granularity = 1024;

-- `value` decreases with `key`, so the first granules hold the largest values and the threshold
-- of `ORDER BY value DESC` is final after the first block.
INSERT INTO t_top_k_first SELECT number, 1000000 - number, toString(number % 100) FROM numbers(200000);

SET use_top_k_dynamic_filtering = 1, use_skip_indexes_for_top_k = 0, use_query_condition_cache = 0, enable_parallel_replicas = 0, max_threads = 1;
SET explain_query_plan_default = 'legacy';

SELECT '-- plan: the conditions are in PREWHERE together with the top-K filter';
SELECT trimLeft(explain) FROM (EXPLAIN actions = 1 SELECT key FROM t_top_k_first WHERE value % 7 = 0 AND s != '' ORDER BY value DESC LIMIT 10)
WHERE explain LIKE '%Prewhere filter column:%' OR explain LIKE '%TopK filter column:%' OR explain LIKE '%Filter column:%';

SELECT '-- plan: an explicit PREWHERE keeps the top-K filter';
SELECT trimLeft(explain) FROM (EXPLAIN actions = 1 SELECT key FROM t_top_k_first PREWHERE value % 7 = 0 WHERE s != '' ORDER BY value DESC LIMIT 10)
WHERE explain LIKE '%Prewhere filter column:%' OR explain LIKE '%TopK filter column:%' OR explain LIKE '%Filter column:%';

SELECT '-- results';
SELECT key, value FROM t_top_k_first WHERE value % 7 = 0 AND s != '' ORDER BY value DESC LIMIT 5;
SELECT key, value FROM t_top_k_first PREWHERE value % 7 = 0 WHERE s != '' ORDER BY value DESC LIMIT 5;
SELECT key, value FROM t_top_k_first WHERE value % 7 = 0 AND s != '' ORDER BY value DESC LIMIT 5 SETTINGS use_top_k_dynamic_filtering = 0;

SELECT key FROM t_top_k_first WHERE value % 7 = 0 AND s != '' ORDER BY value DESC LIMIT 10 FORMAT Null SETTINGS log_comment = '05231_top_k_first_on';
SELECT key FROM t_top_k_first WHERE value % 7 = 0 AND s != '' ORDER BY value DESC LIMIT 10 FORMAT Null SETTINGS log_comment = '05231_top_k_first_off', use_top_k_dynamic_filtering = 0;

SYSTEM FLUSH LOGS query_log;

-- The top-K filter reads `value` for every row, then the conditions read only the surviving rows:
-- the PREWHERE readers read fewer rows in total than without the dynamic filter, where every step reads every row.
SELECT '-- prewhere rows: with the top-K filter < without it';
WITH
    (SELECT ProfileEvents['RowsReadByPrewhereReaders'] FROM system.query_log
        WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment = '05231_top_k_first_on' ORDER BY event_time_microseconds DESC LIMIT 1) AS rows_on,
    (SELECT ProfileEvents['RowsReadByPrewhereReaders'] FROM system.query_log
        WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment = '05231_top_k_first_off' ORDER BY event_time_microseconds DESC LIMIT 1) AS rows_off
SELECT rows_on < rows_off;

DROP TABLE t_top_k_first;
