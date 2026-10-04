-- The text index prefetches depend on the caches: a cold part prefetches its header, a cached header lets the
-- dictionary blocks be prefetched right away, cached tokens let their posting lists be prefetched without the
-- header, and a cached absent token proves the granule empty, so nothing is prefetched. The states are built with
-- per-query cache settings, so other queries do not interfere.

DROP TABLE IF EXISTS t_text_index_prefetch_cache;

CREATE TABLE t_text_index_prefetch_cache
(
    id UInt64,
    message String,
    INDEX idx_message message TYPE text(tokenizer = splitByNonAlpha, dictionary_block_size = 128, posting_list_block_size = 1048576) GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 256, index_granularity_bytes = '100Mi', max_bytes_to_merge_at_max_space_in_pool = 1;

INSERT INTO t_text_index_prefetch_cache
SELECT number, concat('word', toString(number % 1000), ' ', multiIf(number % 100 = 7, 'alpha', number % 100 = 8, 'beta', number % 100 = 9, 'gamma', 'other'))
FROM numbers(100000);

SET enable_analyzer = 1, use_skip_indexes = 1, use_query_condition_cache = 0, max_rows_to_read = 0, enable_parallel_replicas = 0;
SET use_skip_indexes_prefetch = 1, use_reader_executor = 0, use_skip_indexes_on_data_read = 0;
SET local_filesystem_read_method = 'pread_threadpool', local_filesystem_read_prefetch = 1;
SET remote_filesystem_read_method = 'threadpool', remote_filesystem_read_prefetch = 1;
SET use_text_index_header_cache = 1, use_text_index_tokens_cache = 1, use_text_index_negative_tokens_cache = 1, use_text_index_postings_cache = 1;
SET text_index_posting_list_apply_mode = 'materialize', query_plan_direct_read_from_text_index = 0;

-- Cold: the header is prefetched, then the dictionary and the posting list.
SELECT count() FROM t_text_index_prefetch_cache WHERE hasAllTokens(message, ['alpha']) SETTINGS log_comment = 'text_prefetch_cold';

-- The header is cached, the token is not: the dictionary block is prefetched without reading the header.
SELECT count() FROM t_text_index_prefetch_cache WHERE hasAllTokens(message, ['beta']) SETTINGS log_comment = 'text_prefetch_header_cached';

-- The token is cached and its posting list is not: the list is prefetched without the header and the dictionary.
SELECT count() FROM t_text_index_prefetch_cache WHERE hasAllTokens(message, ['alpha'])
SETTINGS use_text_index_postings_cache = 0, log_comment = 'text_prefetch_tokens_cached';

-- A cached absent token proves that the part does not match: nothing of the text index is prefetched.
SELECT count() FROM t_text_index_prefetch_cache WHERE hasAllTokens(message, ['missingtoken']) SETTINGS log_comment = 'text_prefetch_negative_miss';
SELECT count() FROM t_text_index_prefetch_cache WHERE hasAllTokens(message, ['missingtoken']) SETTINGS log_comment = 'text_prefetch_negative_hit';

-- The same results without prefetching.
SELECT count() FROM t_text_index_prefetch_cache WHERE hasAllTokens(message, ['alpha']) SETTINGS use_skip_indexes_prefetch = 0;
SELECT count() FROM t_text_index_prefetch_cache WHERE hasAllTokens(message, ['beta']) SETTINGS use_skip_indexes_prefetch = 0;

SYSTEM FLUSH LOGS query_log;

SELECT
    replaceOne(log_comment, 'text_prefetch_', '') AS state,
    ProfileEvents['TextIndexPrefetchedHeaders'] AS headers,
    ProfileEvents['TextIndexPrefetchedDictionaryBlocks'] > 0 AS dictionary_blocks,
    ProfileEvents['TextIndexPrefetchedPostings'] > 0 AS postings,
    ProfileEvents['TextIndexUnusedPrefetches'] AS unused
FROM system.query_log
WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment LIKE 'text_prefetch_%'
ORDER BY event_time_microseconds;

DROP TABLE t_text_index_prefetch_cache;
