-- Tags: no-fasttest
-- - no-fasttest -- needs S3

-- With the uncompressed cache, a prefetch of the prefetched read pool reads at the position the next read seeks to,
-- so no prefetch is discarded by that seek.

DROP TABLE IF EXISTS t_cached_compressed_prefetch;

CREATE TABLE t_cached_compressed_prefetch (id UInt64, x UInt64, s String)
ENGINE = MergeTree ORDER BY id
SETTINGS storage_policy = 's3_no_cache', index_granularity = 1024, index_granularity_bytes = '100Mi',
    min_bytes_for_wide_part = 0, min_bytes_for_full_part_storage = 0;

INSERT INTO t_cached_compressed_prefetch SELECT number, number * 3, toString(number) FROM numbers(300000);

SELECT sum(x), sum(length(s)) FROM t_cached_compressed_prefetch
SETTINGS use_uncompressed_cache = 1, allow_prefetched_read_pool_for_remote_filesystem = 1,
    remote_filesystem_read_method = 'threadpool', remote_filesystem_read_prefetch = 1, use_reader_executor = 0,
    use_page_cache_for_disks_without_file_cache = 0, use_page_cache_for_object_storage = 0, max_threads = 4,
    merge_tree_read_split_ranges_into_intersecting_and_non_intersecting_injection_probability = 0,
    log_comment = 'cached_compressed_prefetch';

SYSTEM FLUSH LOGS query_log;

SELECT ProfileEvents['RemoteFSPrefetches'] > 0, ProfileEvents['RemoteFSCancelledPrefetches']
FROM system.query_log
WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment = 'cached_compressed_prefetch';

DROP TABLE t_cached_compressed_prefetch;
