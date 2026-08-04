-- Direct read from a text index under plan-based parallel replicas: the initiator defers the
-- __text_index_* rewrite for reads that may be captured into a plan fragment; the local fragment
-- and every remote replica re-optimize their plan instance and apply the rewrite to their own
-- parts. The plan must split for parallel replicas (it used to be kept local), results must match,
-- and reads that stay local (FINAL, IN (subquery)) must still get the rewrite from the top-up pass.

SET enable_analyzer = 1;
SET enable_parallel_replicas = 1;
SET parallel_replicas_for_non_replicated_merge_tree = 1;
SET max_parallel_replicas = 3;
SET cluster_for_parallel_replicas = 'test_cluster_one_shard_three_replicas_localhost';
SET parallel_replicas_plan_based = 1;
SET parallel_replicas_local_plan = 1;
SET automatic_parallel_replicas_mode = 0;
SET use_skip_indexes = 1;
SET use_skip_indexes_on_data_read = 1;
SET query_plan_direct_read_from_text_index = 1;
SET parallel_replicas_min_number_of_rows_per_replica = 0;
SET log_comment = '04758_text_index_direct_read_pr_plan_based';

DROP TABLE IF EXISTS t_text_pr_plan;

CREATE TABLE t_text_pr_plan (id UInt64, s String, INDEX idx s TYPE text(tokenizer = 'splitByNonAlpha'))
ENGINE = MergeTree ORDER BY tuple();

INSERT INTO t_text_pr_plan SELECT number, 'hello world ' || toString(number) FROM numbers(1000);
INSERT INTO t_text_pr_plan SELECT number, 'foo bar ' || toString(number) FROM numbers(1000);

SELECT '-- results match: the same predicate in PREWHERE and WHERE (AST fuzzer shape) and plain WHERE';
SELECT count() FROM t_text_pr_plan PREWHERE hasToken(s, 'hello') WHERE hasToken(s, 'hello');
SELECT count() FROM t_text_pr_plan WHERE hasToken(s, 'hello');
SELECT count() FROM t_text_pr_plan WHERE hasAnyTokens(s, ['hello', 'nonexistent']);

SELECT '-- the plan splits for parallel replicas';
SELECT countIf(explain LIKE '%ReadFromParallelReplicas%') > 0
FROM (EXPLAIN pretty=0, description=0 SELECT count() FROM t_text_pr_plan PREWHERE hasToken(s, 'hello') WHERE hasToken(s, 'hello'));

SELECT '-- the text index is read on the replicas';
SYSTEM FLUSH LOGS query_log;
SELECT sum(ProfileEvents['TextIndexReadPostings'] + ProfileEvents['TextIndexUsedEmbeddedPostings']) > 0
FROM system.query_log
WHERE event_date >= yesterday() AND type = 'QueryFinish'
  AND Settings['log_comment'] = '04758_text_index_direct_read_pr_plan_based';

-- A FINAL read disables parallel replicas for the whole plan; the top-up pass must still apply
-- the direct read to it (visible in EXPLAIN, unlike a captured fragment).
SELECT '-- FINAL stays local and still direct-reads';
DROP TABLE IF EXISTS t_text_pr_final;
CREATE TABLE t_text_pr_final (id UInt64, s String, INDEX idx s TYPE text(tokenizer = 'splitByNonAlpha'))
ENGINE = ReplacingMergeTree ORDER BY id;
INSERT INTO t_text_pr_final SELECT number, 'hello world ' || toString(number) FROM numbers(1000);

SELECT count() FROM t_text_pr_final FINAL WHERE hasToken(s, 'hello');
SELECT countIf(explain LIKE '%ReadFromParallelReplicas%'), countIf(explain LIKE '%__text_index_%') > 0
FROM (EXPLAIN actions=1 SELECT count() FROM t_text_pr_final FINAL WHERE hasToken(s, 'hello'));

-- An IN (subquery) also keeps the plan local; the top-up must apply the direct read.
SELECT '-- IN (subquery) stays local and still direct-reads';
SELECT count() FROM t_text_pr_plan WHERE hasToken(s, 'hello') AND id IN (SELECT number * 2 FROM numbers(100));
SELECT countIf(explain LIKE '%ReadFromParallelReplicas%'), countIf(explain LIKE '%__text_index_%') > 0
FROM (EXPLAIN actions=1 SELECT count() FROM t_text_pr_plan WHERE hasToken(s, 'hello') AND id IN (SELECT number * 2 FROM numbers(100)));

DROP TABLE t_text_pr_final;
DROP TABLE t_text_pr_plan;
