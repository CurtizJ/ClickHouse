-- Tags: no-old-analyzer
-- no-old-analyzer: make_distributed_plan requires the analyzer.

-- Direct read from a text index under make_distributed_plan (issue #109329): the initiator defers
-- the __text_index_* rewrite for a plan that may ship reads; every worker task is re-optimized
-- with make_distributed_plan disabled and applies the rewrite to its own part list. The queries
-- must return the same rows as the non-distributed plan, and the text index must actually be read
-- on the executing tasks.

DROP TABLE IF EXISTS t_text_dp;
CREATE TABLE t_text_dp (id UInt64, s String, INDEX idx_text s TYPE text(tokenizer = 'splitByNonAlpha'))
    ENGINE = MergeTree ORDER BY id;
INSERT INTO t_text_dp SELECT number, 'word' || toString(number) FROM numbers(100000);

SET make_distributed_plan = 1, distributed_plan_execute_locally = 1,
    distributed_plan_max_rows_to_broadcast = 0, distributed_plan_default_reader_bucket_count = 3,
    distributed_plan_default_shuffle_join_bucket_count = 3, max_rows_to_group_by = 0,
    query_plan_direct_read_from_text_index = 1, use_skip_indexes = 1, use_skip_indexes_on_data_read = 1;
SET log_comment = '04657_distributed_plan_text_index_direct_read';

SELECT 'text search over a distributed plan matches single-node';
SELECT count() FROM t_text_dp WHERE hasAnyTokens(s, ['word42']);
SELECT count() FROM t_text_dp WHERE hasAnyTokens(s, ['word42']) SETTINGS make_distributed_plan = 0;
SELECT count() FROM t_text_dp WHERE hasToken(s, 'word4242');
SELECT count() FROM t_text_dp WHERE hasToken(s, 'word4242') SETTINGS make_distributed_plan = 0;
SELECT id FROM t_text_dp PREWHERE hasToken(s, 'word777') WHERE hasToken(s, 'word777');
SELECT id FROM t_text_dp PREWHERE hasToken(s, 'word777') WHERE hasToken(s, 'word777') SETTINGS make_distributed_plan = 0;

SELECT 'the query distributes';
SELECT 'distributes'
FROM (EXPLAIN PIPELINE SELECT count() FROM t_text_dp WHERE hasAnyTokens(s, ['word42']))
WHERE explain LIKE '%ReadFromDistributedPlanSource%' LIMIT 1;

-- The single-stage shape (all parts pruned by the negative LIMIT) is executed locally by the
-- re-optimization in QueryPlan::convertToDistributed, which applies the deferred rewrite.
SELECT 'single stage still runs';
SELECT id FROM t_text_dp
PREWHERE (materialize(65537) >= id) AND hasToken(s, '')
WHERE xor(hasToken(s, ''), (id >= 65537))
LIMIT -2147483649;
SELECT 'ok';

-- The initiator defers the rewrite, so its own optimization logs no 'Added:' line for a
-- distributed query; every 'Added: [__text_index_...]' under that query's id comes from a worker
-- task re-optimizing its fragment (task threads share the initiator's query_id). This asserts the
-- rewrite really fired on the executing tasks; per-task ProfileEvents are not usable here because
-- reading threads of locally executed tasks are not attributed to the task's query_log entries.
SELECT 'the rewrite fires on the worker tasks';
SYSTEM FLUSH LOGS text_log, query_log;
SELECT count() > 0
FROM system.text_log
WHERE event_date >= yesterday()
  AND logger_name = 'processAndOptimizeTextIndexFunctions'
  AND startsWith(message, 'Added:')
  AND query_id = (
      SELECT query_id FROM system.query_log
      WHERE event_date >= yesterday() AND type = 'QueryFinish'
        AND current_database = currentDatabase()
        AND Settings['log_comment'] = '04657_distributed_plan_text_index_direct_read'
        AND Settings['make_distributed_plan'] = '1'
        AND query LIKE '%hasAnyTokens%' AND query NOT LIKE '%text_log%' AND query NOT LIKE '%EXPLAIN%'
      ORDER BY event_time_microseconds DESC
      LIMIT 1)
SETTINGS make_distributed_plan = 0, max_rows_to_read = 0;

DROP TABLE t_text_dp;
