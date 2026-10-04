-- Tags: no-fasttest
-- - no-fasttest -- needs S3

-- Text index prefetching on S3 does not add requests: near dictionary blocks share one request, and a dictionary scan
-- reads each contiguous range of blocks with one request, as without prefetching. Query-local text index caches keep
-- every query cold.

DROP TABLE IF EXISTS t_text_index_prefetch_s3;

CREATE TABLE t_text_index_prefetch_s3
(
    id UInt64,
    message String,
    INDEX idx_message message TYPE text(tokenizer = splitByNonAlpha, dictionary_block_size = 128, posting_list_block_size = 1048576) GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id
SETTINGS storage_policy = 's3_no_cache', index_granularity = 256, index_granularity_bytes = '100Mi',
    max_bytes_to_merge_at_max_space_in_pool = 1, min_bytes_for_wide_part = 0;

INSERT INTO t_text_index_prefetch_s3
SELECT number, concat('word', toString(number % 5000), ' ', multiIf(number % 100 = 7, 'alpha', number % 100 = 8, 'beta', number % 100 = 9, 'gamma', 'other'))
FROM numbers(0, 50000);
INSERT INTO t_text_index_prefetch_s3
SELECT number, concat('word', toString(number % 5000), ' ', multiIf(number % 100 = 7, 'alpha', number % 100 = 8, 'beta', number % 100 = 9, 'gamma', 'other'))
FROM numbers(50000, 50000);

SET enable_analyzer = 1, use_skip_indexes = 1, use_query_condition_cache = 0, max_rows_to_read = 0, enable_parallel_replicas = 0;
SET remote_filesystem_read_method = 'threadpool', remote_filesystem_read_prefetch = 1, use_reader_executor = 0;
SET use_page_cache_for_disks_without_file_cache = 0, use_page_cache_for_object_storage = 0;
SET merge_tree_read_split_ranges_into_intersecting_and_non_intersecting_injection_probability = 0;
SET use_text_index_header_cache = 0, use_text_index_tokens_cache = 0, use_text_index_postings_cache = 0;
SET query_plan_direct_read_from_text_index = 0, use_text_index_like_evaluation_by_dictionary_scan = 1;
SET max_threads = 2;

SELECT count() FROM t_text_index_prefetch_s3 WHERE hasAllTokens(message, ['alpha']) SETTINGS use_skip_indexes_prefetch = 0, log_comment = 'text_s3_one_off';
SELECT count() FROM t_text_index_prefetch_s3 WHERE hasAllTokens(message, ['alpha']) SETTINGS use_skip_indexes_prefetch = 1, log_comment = 'text_s3_one_on';

SELECT count() FROM t_text_index_prefetch_s3 WHERE hasAnyTokens(message, ['alpha', 'beta', 'gamma', 'word17', 'word4242'])
SETTINGS use_skip_indexes_prefetch = 0, log_comment = 'text_s3_five_off';
SELECT count() FROM t_text_index_prefetch_s3 WHERE hasAnyTokens(message, ['alpha', 'beta', 'gamma', 'word17', 'word4242'])
SETTINGS use_skip_indexes_prefetch = 1, log_comment = 'text_s3_five_on';

SELECT count() FROM t_text_index_prefetch_s3 WHERE message LIKE '%ord123%' SETTINGS use_skip_indexes_prefetch = 0, log_comment = 'text_s3_like_off';
SELECT count() FROM t_text_index_prefetch_s3 WHERE message LIKE '%ord123%' SETTINGS use_skip_indexes_prefetch = 1, log_comment = 'text_s3_like_on';

SYSTEM FLUSH LOGS query_log;

SELECT
    replaceRegexpOne(o.log_comment, '_on$', '') AS shape,
    o.dictionary_blocks > 0,
    o.gets <= f.gets
FROM
(
    SELECT
        log_comment,
        ProfileEvents['TextIndexPrefetchedDictionaryBlocks'] AS dictionary_blocks,
        ProfileEvents['S3GetObject'] AS gets
    FROM system.query_log
    WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment LIKE 'text_s3_%_on'
) AS o
INNER JOIN
(
    SELECT
        replaceRegexpOne(log_comment, '_off$', '_on') AS on_comment,
        ProfileEvents['S3GetObject'] AS gets
    FROM system.query_log
    WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment LIKE 'text_s3_%_off'
) AS f ON o.log_comment = f.on_comment
ORDER BY shape;

DROP TABLE t_text_index_prefetch_s3;
