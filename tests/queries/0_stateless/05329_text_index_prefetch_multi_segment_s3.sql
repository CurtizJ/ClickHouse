-- Tags: no-fasttest
-- - no-fasttest -- needs S3

-- On S3, prefetching the segments of multi-segment posting lists does not add requests: each list is read on its own
-- stream, sequentially. Two identical tables keep both runs cold.

DROP TABLE IF EXISTS t_text_index_prefetch_segments_s3_off;
DROP TABLE IF EXISTS t_text_index_prefetch_segments_s3_on;

CREATE TABLE t_text_index_prefetch_segments_s3_off
(
    id UInt64,
    message String,
    INDEX idx_message message TYPE text(tokenizer = splitByNonAlpha, posting_list_block_size = 1024, posting_list_codec = 'bitpacking') GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id
SETTINGS storage_policy = 's3_no_cache', index_granularity = 1024, index_granularity_bytes = '100Mi',
    max_bytes_to_merge_at_max_space_in_pool = 1, min_bytes_for_wide_part = 0;

CREATE TABLE t_text_index_prefetch_segments_s3_on AS t_text_index_prefetch_segments_s3_off;

INSERT INTO t_text_index_prefetch_segments_s3_off
SELECT number, multiIf(number % 3 = 0, 'common often word', number % 3 = 1, 'often common word', 'common other word') FROM numbers(100000);
INSERT INTO t_text_index_prefetch_segments_s3_on
SELECT number, multiIf(number % 3 = 0, 'common often word', number % 3 = 1, 'often common word', 'common other word') FROM numbers(100000);

SET enable_analyzer = 1, use_skip_indexes = 1, use_query_condition_cache = 0, max_rows_to_read = 0, enable_parallel_replicas = 0;
SET remote_filesystem_read_method = 'threadpool', remote_filesystem_read_prefetch = 1, use_reader_executor = 0;
SET use_page_cache_for_disks_without_file_cache = 0, use_page_cache_for_object_storage = 0;
SET merge_tree_read_split_ranges_into_intersecting_and_non_intersecting_injection_probability = 0;
SET max_threads = 2, query_plan_direct_read_from_text_index = 1, use_text_index_postings_cache = 0, text_index_posting_list_apply_mode = 'lazy';

SELECT sum(id) FROM t_text_index_prefetch_segments_s3_off WHERE hasAllTokens(message, ['common', 'often'])
SETTINGS use_skip_indexes_prefetch = 0, log_comment = 'text_segments_s3_off';
SELECT sum(id) FROM t_text_index_prefetch_segments_s3_on WHERE hasAllTokens(message, ['common', 'often'])
SETTINGS use_skip_indexes_prefetch = 1, log_comment = 'text_segments_s3_on';

SYSTEM FLUSH LOGS query_log;

SELECT o.prefetched > 0, o.gets <= f.gets
FROM
(
    SELECT ProfileEvents['TextIndexPrefetchedPostings'] AS prefetched, ProfileEvents['S3GetObject'] AS gets
    FROM system.query_log
    WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment = 'text_segments_s3_on'
) AS o,
(
    SELECT ProfileEvents['S3GetObject'] AS gets
    FROM system.query_log
    WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment = 'text_segments_s3_off'
) AS f;

DROP TABLE t_text_index_prefetch_segments_s3_off;
DROP TABLE t_text_index_prefetch_segments_s3_on;
