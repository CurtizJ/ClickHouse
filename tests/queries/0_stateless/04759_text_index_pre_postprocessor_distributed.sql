-- Tags: no-old-analyzer
-- no-old-analyzer: make_distributed_plan requires the analyzer.

-- Text indexes with a preprocessor or a postprocessor under plan-shipping execution modes.
-- The plan ships with the original predicate: the tokenizer/preprocessor rewrite bakes node-local
-- index metadata into the function arguments, so it must run only on the plan instance that
-- executes the read. Every executing instance (a distributed-plan task, a parallel replica) then
-- matches the original form against its own index metadata and applies both the rewrite and the
-- direct read.

SET enable_analyzer = 1;
SET use_skip_indexes = 1, use_skip_indexes_on_data_read = 1, query_plan_direct_read_from_text_index = 1;

DROP TABLE IF EXISTS t_text_prep_dist;
CREATE TABLE t_text_prep_dist (id UInt64, s String,
    INDEX idx(s) TYPE text(tokenizer = splitByNonAlpha, preprocessor = lower(s)))
ENGINE = MergeTree ORDER BY id;
INSERT INTO t_text_prep_dist SELECT number, concat('Hello World ', if(number % 3000 = 0, 'NEEDLE', 'straw')) FROM numbers(100000);

DROP TABLE IF EXISTS t_text_post_dist;
CREATE TABLE t_text_post_dist (id UInt64, s String,
    INDEX idx(s) TYPE text(tokenizer = splitByNonAlpha, postprocessor = replaceRegexpAll(s, 'ing$', ''), support_phrase_search = 1))
ENGINE = MergeTree ORDER BY id
SETTINGS allow_experimental_text_index_phrase_search = 1;
INSERT INTO t_text_post_dist SELECT number, concat('some ', if(number % 3000 = 0, 'matching', 'other'), ' words') FROM numbers(100000);

SELECT '-- preprocessor: distributed plan matches single-node';
SET log_comment = '04759_text_index_preprocessor_mdp';
SELECT count() FROM t_text_prep_dist WHERE hasToken(s, 'needle')
SETTINGS make_distributed_plan = 1, distributed_plan_execute_locally = 1, distributed_plan_default_reader_bucket_count = 3;
SELECT count() FROM t_text_prep_dist WHERE hasToken(s, 'needle') SETTINGS make_distributed_plan = 0;
SELECT count() FROM t_text_prep_dist WHERE hasAnyTokens(s, 'NEEDLE world')
SETTINGS make_distributed_plan = 1, distributed_plan_execute_locally = 1, distributed_plan_default_reader_bucket_count = 3;
SELECT count() FROM t_text_prep_dist WHERE hasAnyTokens(s, 'NEEDLE world') SETTINGS make_distributed_plan = 0;

SELECT '-- postprocessor: distributed plan matches single-node';
SELECT count() FROM t_text_post_dist WHERE hasToken(s, 'matching')
SETTINGS make_distributed_plan = 1, distributed_plan_execute_locally = 1, distributed_plan_default_reader_bucket_count = 3;
SELECT count() FROM t_text_post_dist WHERE hasToken(s, 'matching') SETTINGS make_distributed_plan = 0;
SELECT count() FROM t_text_post_dist WHERE hasPhrase(s, 'some matching words')
SETTINGS make_distributed_plan = 1, distributed_plan_execute_locally = 1, distributed_plan_default_reader_bucket_count = 3;
SELECT count() FROM t_text_post_dist WHERE hasPhrase(s, 'some matching words') SETTINGS make_distributed_plan = 0;

-- The initiator ships the original predicate, so its own optimization logs no 'Added:' line;
-- every 'Added: [__text_index_...]' under the query's id comes from a worker task re-optimizing
-- its fragment against its own index metadata (task threads share the initiator's query_id).
SELECT '-- the rewrite fires on the worker tasks for the preprocessor index';
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
        AND Settings['log_comment'] = '04759_text_index_preprocessor_mdp'
        AND Settings['make_distributed_plan'] = '1'
        AND query LIKE '%hasToken%' AND query NOT LIKE '%text_log%'
      ORDER BY event_time_microseconds DESC
      LIMIT 1)
SETTINGS max_rows_to_read = 0;

SELECT '-- pre/postprocessor: parallel replicas match single-node and read the index';
SET log_comment = '04759_text_index_preprocessor_pr';
SELECT count() FROM t_text_prep_dist WHERE hasToken(s, 'needle')
SETTINGS enable_parallel_replicas = 1, parallel_replicas_plan_based = 1, parallel_replicas_local_plan = 1,
    max_parallel_replicas = 3, cluster_for_parallel_replicas = 'test_cluster_one_shard_three_replicas_localhost',
    parallel_replicas_for_non_replicated_merge_tree = 1, parallel_replicas_min_number_of_rows_per_replica = 0,
    automatic_parallel_replicas_mode = 0;
SELECT count() FROM t_text_post_dist WHERE hasToken(s, 'matching')
SETTINGS enable_parallel_replicas = 1, parallel_replicas_plan_based = 1, parallel_replicas_local_plan = 1,
    max_parallel_replicas = 3, cluster_for_parallel_replicas = 'test_cluster_one_shard_three_replicas_localhost',
    parallel_replicas_for_non_replicated_merge_tree = 1, parallel_replicas_min_number_of_rows_per_replica = 0,
    automatic_parallel_replicas_mode = 0;
SYSTEM FLUSH LOGS query_log;
SELECT sum(ProfileEvents['TextIndexReadPostings'] + ProfileEvents['TextIndexUsedEmbeddedPostings']) > 0
FROM system.query_log
WHERE event_date >= yesterday() AND type = 'QueryFinish'
  AND Settings['log_comment'] = '04759_text_index_preprocessor_pr'
SETTINGS max_rows_to_read = 0;

DROP TABLE t_text_prep_dist;
DROP TABLE t_text_post_dist;
