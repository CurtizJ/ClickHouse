-- Tags: no-parallel, no-random-settings, no-random-merge-tree-settings, no-replicated-database, no-parallel-replicas
-- A direct-read text-index `LIKE` query whose dictionary scan is cut short evaluates
-- the predicate on the physical column. The column is read by an ordinary reader
-- of the PREWHERE step that uses it, so it goes through the columns cache like any
-- other read: the first query writes `s` to the cache and the second one reads it
-- from there. The read pool accounts for `s` in the query's columns cache write
-- estimate, although the decision to read it is made at read time.

SET enable_analyzer = 1;
SET max_threads = 1;

-- Force the direct read from the text index; CI may inject these as false, in
-- which case the query would just scan `s` and never reach the fallback reader.
SET use_skip_indexes = 1;
SET use_skip_indexes_on_data_read = 1;
SET query_plan_direct_read_from_text_index = 1;
SET query_plan_text_index_add_hint = 1;
SET use_text_index_like_evaluation_by_dictionary_scan = 1;

-- Abandon the dictionary scan on the first token with a non-embedded posting
-- list, so the pattern query is bypassed and is evaluated on the physical column.
SET text_index_like_max_postings_to_read = 0;
SET use_text_index_pattern_bypass_cache = 0;

DROP TABLE IF EXISTS t_cache_text_fallback;

CREATE TABLE t_cache_text_fallback
(
    id UInt64,
    s String,
    INDEX idx_s s TYPE text(tokenizer = splitByNonAlpha) GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id
SETTINGS min_bytes_for_wide_part = 0, index_granularity = 1000;

INSERT INTO t_cache_text_fallback SELECT number, concat('token', toString(number % 100), ' payload') FROM numbers(10000);

SYSTEM DROP COLUMNS CACHE;

-- An ordinary read that populates the cache for `id`, proving the cache is
-- active in this environment (the assertion on `s` below is not vacuous).
SELECT sum(id) FROM t_cache_text_fallback SETTINGS use_columns_cache = 1;

-- The direct-read text-index query: the truncated dictionary scan bypasses the
-- pattern query, so `s` is read by an ordinary reader. The first query writes it
-- to the cache, the second one reads it from there.
SELECT count() FROM t_cache_text_fallback WHERE s LIKE '%token4%'
SETTINGS use_columns_cache = 1, log_comment = 'columns_cache_text_index_fallback_1';

SELECT count() FROM t_cache_text_fallback WHERE s LIKE '%token4%'
SETTINGS use_columns_cache = 1, log_comment = 'columns_cache_text_index_fallback_2';

-- The same query without the index must return the same count.
SELECT count() FROM t_cache_text_fallback WHERE s LIKE '%token4%'
SETTINGS use_skip_indexes = 0, use_columns_cache = 0;

-- The cache holds entries for `id` and for `s`.
SELECT countIf(column = 'id') > 0, countIf(column = 's') > 0
FROM system.columns_cache
WHERE database = currentDatabase() AND table = 't_cache_text_fallback';

-- The read pool counts `s` in the write estimate before it knows that the pattern is evaluated from it,
-- so a tiny estimate budget disables the writes.
SYSTEM DROP COLUMNS CACHE;
SELECT count() FROM t_cache_text_fallback WHERE s LIKE '%token4%'
SETTINGS use_columns_cache = 1, columns_cache_max_estimated_bytes_to_write_to_cache = 1;

SELECT count()
FROM system.columns_cache
WHERE database = currentDatabase() AND table = 't_cache_text_fallback';

SYSTEM FLUSH LOGS query_log;

-- Both queries abandoned the dictionary scan and evaluated the pattern from the predicate. The first one missed
-- the cache for `s`, the second one hit it.
SELECT
    log_comment,
    ProfileEvents['TextIndexDiscardPatternScan'] > 0,
    ProfileEvents['TextIndexDirectReadFallbackColumns'] > 0,
    ProfileEvents['ColumnsCacheMisses'] > 0,
    ProfileEvents['ColumnsCacheHits'] > 0
FROM system.query_log
WHERE current_database = currentDatabase()
    AND type = 'QueryFinish'
    AND event_date >= yesterday()
    AND log_comment LIKE 'columns_cache_text_index_fallback_%'
ORDER BY log_comment;

DROP TABLE t_cache_text_fallback;
