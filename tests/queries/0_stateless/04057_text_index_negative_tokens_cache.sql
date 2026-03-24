-- Tags: no-parallel, no-parallel-replicas
-- no-parallel: looks at server-wide metrics

--- These tests verify the caching of negative entries (token not found) in the text index tokens cache.

SET enable_analyzer = 1;
SET use_skip_indexes_on_data_read = 1;
SET query_plan_direct_read_from_text_index = 1;
SET use_text_index_tokens_cache = 1;
SET log_queries = 1;
SET max_rows_to_read = 0;

SYSTEM CLEAR TEXT INDEX TOKENS CACHE;

DROP TABLE IF EXISTS tab;
CREATE TABLE tab
(
    id UInt32,
    message String,
    INDEX idx(message) TYPE text(tokenizer = array, dictionary_block_size = 128) GRANULARITY 1
)
ENGINE = MergeTree
ORDER BY (id)
SETTINGS index_granularity = 128;

INSERT INTO tab
SELECT
    number,
    concat('text_', leftPad(toString(number), 3, '0'))
FROM numbers(256);

DROP VIEW IF EXISTS text_index_cache_stats;
CREATE VIEW text_index_cache_stats AS (
  SELECT
    concat('cache_hits = ', toString(ProfileEvents['TextIndexTokensCacheHits']), ', cache_misses = ', toString(ProfileEvents['TextIndexTokensCacheMisses']))
  FROM system.query_log
  WHERE event_date >= yesterday() AND event_time >= now() - 600 AND query_kind ='Select'
      AND current_database = currentDatabase()
      AND endsWith(trimRight(query), concat('hasAnyTokens(message, \'', {filter:String}, '\');'))
      AND type='QueryFinish'
  ORDER BY event_time_microseconds DESC
  LIMIT 1
);

SELECT '--- cache miss on a non-existent token.';
SELECT count() FROM tab WHERE hasAnyTokens(message, 'nonexistent_token');

SYSTEM FLUSH LOGS query_log;
SELECT * FROM text_index_cache_stats(filter = 'nonexistent_token');

SELECT '--- negative cache hit on the same non-existent token.';
SELECT count() FROM tab WHERE hasAnyTokens(message, 'nonexistent_token');

SYSTEM FLUSH LOGS query_log;
SELECT * FROM text_index_cache_stats(filter = 'nonexistent_token');

SELECT '--- cache miss on an existing token.';
SELECT count() FROM tab WHERE hasAnyTokens(message, 'text_000');

SYSTEM FLUSH LOGS query_log;
SELECT * FROM text_index_cache_stats(filter = 'text_000');

SELECT '--- positive cache hit on the same existing token.';
SELECT count() FROM tab WHERE hasAnyTokens(message, 'text_000');

SYSTEM FLUSH LOGS query_log;
SELECT * FROM text_index_cache_stats(filter = 'text_000');

SELECT '--- negative cache hit after clearing and re-caching.';
SYSTEM CLEAR TEXT INDEX TOKENS CACHE;

SELECT count() FROM tab WHERE hasAnyTokens(message, 'nonexistent_token');

SYSTEM FLUSH LOGS query_log;
SELECT * FROM text_index_cache_stats(filter = 'nonexistent_token');

SELECT count() FROM tab WHERE hasAnyTokens(message, 'nonexistent_token');

SYSTEM FLUSH LOGS query_log;
SELECT * FROM text_index_cache_stats(filter = 'nonexistent_token');

SYSTEM CLEAR TEXT INDEX TOKENS CACHE;
DROP TABLE tab;
