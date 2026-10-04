-- Tags: no-fasttest
-- - no-fasttest -- needs S3

-- The look-ahead of the read pools on S3 prefetches skip indexes without adding S3 requests.
-- Two identical tables keep both runs cold (the mark caches are keyed by file).

DROP TABLE IF EXISTS t_skip_index_prefetch_data_read_s3_off;
DROP TABLE IF EXISTS t_skip_index_prefetch_data_read_s3_on;

CREATE TABLE t_skip_index_prefetch_data_read_s3_off
(
    id UInt64,
    v UInt64,
    s String,
    INDEX idx_v v TYPE minmax GRANULARITY 1,
    INDEX idx_s s TYPE bloom_filter GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id
SETTINGS storage_policy = 's3_no_cache', index_granularity = 256, index_granularity_bytes = '100Mi',
    max_bytes_to_merge_at_max_space_in_pool = 1, min_bytes_for_wide_part = 0, min_bytes_for_full_part_storage = 0;

CREATE TABLE t_skip_index_prefetch_data_read_s3_on AS t_skip_index_prefetch_data_read_s3_off;

INSERT INTO t_skip_index_prefetch_data_read_s3_off SELECT number, number % 10000, concat('filler_', toString(number % 3000)) FROM numbers(0, 20000);
INSERT INTO t_skip_index_prefetch_data_read_s3_off SELECT number, number % 10000, concat('filler_', toString(number % 3000)) FROM numbers(20000, 20000);
INSERT INTO t_skip_index_prefetch_data_read_s3_off SELECT number, number % 10000, concat('filler_', toString(number % 3000)) FROM numbers(40000, 20000);
INSERT INTO t_skip_index_prefetch_data_read_s3_off SELECT number, number % 10000, concat('filler_', toString(number % 3000)) FROM numbers(60000, 20000);

INSERT INTO t_skip_index_prefetch_data_read_s3_on SELECT number, number % 10000, concat('filler_', toString(number % 3000)) FROM numbers(0, 20000);
INSERT INTO t_skip_index_prefetch_data_read_s3_on SELECT number, number % 10000, concat('filler_', toString(number % 3000)) FROM numbers(20000, 20000);
INSERT INTO t_skip_index_prefetch_data_read_s3_on SELECT number, number % 10000, concat('filler_', toString(number % 3000)) FROM numbers(40000, 20000);
INSERT INTO t_skip_index_prefetch_data_read_s3_on SELECT number, number % 10000, concat('filler_', toString(number % 3000)) FROM numbers(60000, 20000);

SET enable_analyzer = 1, use_skip_indexes = 1, use_skip_indexes_on_data_read = 1, use_query_condition_cache = 0, max_rows_to_read = 0;
SET enable_parallel_replicas = 0, use_reader_executor = 0, max_threads = 2;
SET remote_filesystem_read_method = 'threadpool', remote_filesystem_read_prefetch = 1;
SET use_page_cache_for_disks_without_file_cache = 0, use_page_cache_for_object_storage = 0;
SET merge_tree_read_split_ranges_into_intersecting_and_non_intersecting_injection_probability = 0;
SET allow_prefetched_read_pool_for_remote_filesystem = 0, use_indexes_refiner_in_read_pools = 0;

SELECT count(), sum(v) FROM t_skip_index_prefetch_data_read_s3_off WHERE v >= 2000 AND s = 'filler_42'
SETTINGS use_skip_indexes_prefetch = 0, log_comment = 'skip_index_prefetch_data_read_s3_off';

SELECT count(), sum(v) FROM t_skip_index_prefetch_data_read_s3_on WHERE v >= 2000 AND s = 'filler_42'
SETTINGS use_skip_indexes_prefetch = 1, log_comment = 'skip_index_prefetch_data_read_s3_on';

SYSTEM FLUSH LOGS query_log;

SELECT o.prefetches > 0, o.gets <= f.gets
FROM
(
    SELECT ProfileEvents['SkipIndexPrefetches'] AS prefetches, ProfileEvents['S3GetObject'] AS gets
    FROM system.query_log
    WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment = 'skip_index_prefetch_data_read_s3_on'
) AS o,
(
    SELECT ProfileEvents['S3GetObject'] AS gets
    FROM system.query_log
    WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment = 'skip_index_prefetch_data_read_s3_off'
) AS f;

DROP TABLE t_skip_index_prefetch_data_read_s3_off;
DROP TABLE t_skip_index_prefetch_data_read_s3_on;
