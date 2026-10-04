-- The text index reader prefetches the multi-segment posting lists it reads, segment after segment, and the
-- positions of phrase tokens. The results are the same without prefetching.

DROP TABLE IF EXISTS t_text_index_prefetch_segments;

CREATE TABLE t_text_index_prefetch_segments
(
    id UInt64,
    message String,
    INDEX idx_message message TYPE text(tokenizer = splitByNonAlpha, posting_list_block_size = 1024, posting_list_codec = 'bitpacking', support_phrase_search = 1) GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 1024, index_granularity_bytes = '100Mi', max_bytes_to_merge_at_max_space_in_pool = 1, allow_experimental_text_index_phrase_search = 1;

INSERT INTO t_text_index_prefetch_segments
SELECT number, multiIf(number % 3 = 0, 'common often word', number % 3 = 1, 'often common word', 'common other word')
FROM numbers(100000);

SET enable_analyzer = 1, use_skip_indexes = 1, use_query_condition_cache = 0, max_rows_to_read = 0, enable_parallel_replicas = 0;
SET use_reader_executor = 0, max_threads = 2, query_plan_direct_read_from_text_index = 1;
SET local_filesystem_read_method = 'pread_threadpool', local_filesystem_read_prefetch = 1;
SET remote_filesystem_read_method = 'threadpool', remote_filesystem_read_prefetch = 1;
SET use_text_index_postings_cache = 0;

SELECT sum(id) FROM t_text_index_prefetch_segments WHERE hasAllTokens(message, ['common', 'often'])
SETTINGS text_index_posting_list_apply_mode = 'lazy', use_skip_indexes_prefetch = 1, log_comment = 'text_segments_lazy_on';
SELECT sum(id) FROM t_text_index_prefetch_segments WHERE hasAllTokens(message, ['common', 'often'])
SETTINGS text_index_posting_list_apply_mode = 'lazy', use_skip_indexes_prefetch = 0, log_comment = 'text_segments_lazy_off';

SELECT sum(id) FROM t_text_index_prefetch_segments WHERE hasAnyTokens(message, ['often', 'other'])
SETTINGS text_index_posting_list_apply_mode = 'materialize', use_skip_indexes_prefetch = 1, log_comment = 'text_segments_materialize_on';
SELECT sum(id) FROM t_text_index_prefetch_segments WHERE hasAnyTokens(message, ['often', 'other'])
SETTINGS text_index_posting_list_apply_mode = 'materialize', use_skip_indexes_prefetch = 0, log_comment = 'text_segments_materialize_off';

SELECT sum(id) FROM t_text_index_prefetch_segments WHERE hasPhrase(message, 'common often')
SETTINGS use_skip_indexes_prefetch = 1, text_index_hint_max_selectivity = 1, log_comment = 'text_segments_phrase_on';
SELECT sum(id) FROM t_text_index_prefetch_segments WHERE hasPhrase(message, 'common often')
SETTINGS use_skip_indexes_prefetch = 0, text_index_hint_max_selectivity = 1, log_comment = 'text_segments_phrase_off';

SYSTEM FLUSH LOGS query_log;

SELECT
    replaceOne(log_comment, 'text_segments_', '') AS name,
    ProfileEvents['TextIndexPrefetchedPostings'] > 0
FROM system.query_log
WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment LIKE 'text_segments_%'
ORDER BY name;

DROP TABLE t_text_index_prefetch_segments;
