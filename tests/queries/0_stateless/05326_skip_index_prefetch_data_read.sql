-- With skip indexes applied on data read, the read pools prefetch the first skip index of the part a thread reads next.
-- The prefetch is I/O only: the results, including the pruning by JOIN runtime filters, are the same without it.

DROP TABLE IF EXISTS t_skip_index_prefetch_data_read;
DROP TABLE IF EXISTS t_skip_index_prefetch_data_read_keys;

CREATE TABLE t_skip_index_prefetch_data_read
(
    id UInt64,
    v UInt64,
    s String,
    INDEX idx_v v TYPE minmax GRANULARITY 1,
    INDEX idx_s s TYPE bloom_filter GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 256, index_granularity_bytes = '100Mi', max_bytes_to_merge_at_max_space_in_pool = 1;

INSERT INTO t_skip_index_prefetch_data_read SELECT number, number % 20000, concat('filler_', toString(number % 3000)) FROM numbers(0, 20000);
INSERT INTO t_skip_index_prefetch_data_read SELECT number, number % 20000, concat('filler_', toString(number % 3000)) FROM numbers(20000, 20000);
INSERT INTO t_skip_index_prefetch_data_read SELECT number, number % 20000, concat('filler_', toString(number % 3000)) FROM numbers(40000, 20000);
INSERT INTO t_skip_index_prefetch_data_read SELECT number, number % 20000, concat('filler_', toString(number % 3000)) FROM numbers(60000, 20000);
INSERT INTO t_skip_index_prefetch_data_read SELECT number, number % 20000, concat('filler_', toString(number % 3000)) FROM numbers(80000, 20000);
INSERT INTO t_skip_index_prefetch_data_read SELECT number, number % 20000, concat('filler_', toString(number % 3000)) FROM numbers(100000, 20000);

CREATE TABLE t_skip_index_prefetch_data_read_keys (s String) ENGINE = MergeTree ORDER BY s;
INSERT INTO t_skip_index_prefetch_data_read_keys VALUES ('filler_42'), ('filler_77');

SET enable_analyzer = 1, use_skip_indexes = 1, use_skip_indexes_on_data_read = 1, use_query_condition_cache = 0, max_rows_to_read = 0;
SET enable_parallel_replicas = 0, use_reader_executor = 0, max_threads = 2;
SET local_filesystem_read_method = 'pread_threadpool', local_filesystem_read_prefetch = 1;
SET remote_filesystem_read_method = 'threadpool', remote_filesystem_read_prefetch = 1;
SET merge_tree_read_split_ranges_into_intersecting_and_non_intersecting_injection_probability = 0;
SET allow_prefetched_read_pool_for_local_filesystem = 0, allow_prefetched_read_pool_for_remote_filesystem = 0;

SELECT count(), sum(v) FROM t_skip_index_prefetch_data_read WHERE v >= 5000 AND s = 'filler_42'
SETTINGS use_skip_indexes_prefetch = 1, log_comment = 'prefetch_data_read_on';
SELECT count(), sum(v) FROM t_skip_index_prefetch_data_read WHERE v >= 5000 AND s = 'filler_42'
SETTINGS use_skip_indexes_prefetch = 0, log_comment = 'prefetch_data_read_off';

-- The prefetched read pool prefetches the indexes of the parts of its tasks.
SELECT count(), sum(v) FROM t_skip_index_prefetch_data_read WHERE v >= 5000 AND s = 'filler_42'
SETTINGS use_skip_indexes_prefetch = 1, allow_prefetched_read_pool_for_local_filesystem = 1, allow_prefetched_read_pool_for_remote_filesystem = 1,
    log_comment = 'prefetch_data_read_prefetched_pool_on';

-- JOIN runtime filters are applied when the result of a part is built, after the prefetch.
SELECT count(), sum(v) FROM t_skip_index_prefetch_data_read AS t INNER JOIN t_skip_index_prefetch_data_read_keys AS k ON t.s = k.s
SETTINGS use_skip_indexes_prefetch = 1, enable_join_runtime_filters = 1, enable_join_runtime_filters_index_analysis = 1, join_runtime_filter_min_probe_rows = 0, query_plan_optimize_join_order_randomize = 0;
SELECT count(), sum(v) FROM t_skip_index_prefetch_data_read AS t INNER JOIN t_skip_index_prefetch_data_read_keys AS k ON t.s = k.s
SETTINGS use_skip_indexes_prefetch = 0, enable_join_runtime_filters = 1, enable_join_runtime_filters_index_analysis = 1, join_runtime_filter_min_probe_rows = 0, query_plan_optimize_join_order_randomize = 0;

SYSTEM FLUSH LOGS query_log;

SELECT
    replaceOne(log_comment, 'prefetch_data_read_', '') AS name,
    ProfileEvents['SkipIndexPrefetches'] > 0
FROM system.query_log
WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment LIKE 'prefetch_data_read_%'
ORDER BY name;

DROP TABLE t_skip_index_prefetch_data_read;
DROP TABLE t_skip_index_prefetch_data_read_keys;
