-- Tags: no-parallel-replicas
-- no-parallel-replicas: the test checks per-query ProfileEvents.

-- The text index reader keeps the decoded segments of the `.dl` (document lengths) substream in the
-- postings cache, so the readers of one query share them and a repeated query does not read them
-- again. With the global cache the second run of the same query must be served from it.

SET enable_analyzer = 1;
SET allow_experimental_bm25_scoring = 1;
SET query_plan_direct_read_from_text_index = 1;
SET use_skip_indexes_on_data_read = 1;
SET use_text_index_postings_cache = 1;
SET log_queries = 1;

DROP TABLE IF EXISTS tab_doc_lengths_cache;

CREATE TABLE tab_doc_lengths_cache
(
    id UInt32,
    body String,
    INDEX idx_body(body) TYPE text(tokenizer = 'splitByNonAlpha', posting_list_codec = 'bitpacking', enable_scoring = 1) GRANULARITY 1
)
ENGINE = MergeTree
ORDER BY id
SETTINGS index_granularity = 128, allow_experimental_text_index_scoring = 1;

SYSTEM STOP MERGES tab_doc_lengths_cache;
SYSTEM DROP TEXT INDEX POSTINGS CACHE;

INSERT INTO tab_doc_lengths_cache
SELECT number, concat('filler text ', toString(number), if(number % 37 = 0, ' raft', '')) FROM numbers(20000);

SELECT '-- the same query twice; scores must not depend on where the segments come from';
SELECT count(), round(sum(bm25()), 4) FROM tab_doc_lengths_cache WHERE hasAnyTokens(body, 'raft')
SETTINGS log_comment = '05157_first';
SELECT count(), round(sum(bm25()), 4) FROM tab_doc_lengths_cache WHERE hasAnyTokens(body, 'raft')
SETTINGS log_comment = '05157_second';

SYSTEM FLUSH LOGS query_log;

SELECT '-- the first query fills the cache, the second one hits it';
SELECT
    replaceOne(log_comment, '05157_', ''),
    ProfileEvents['TextIndexPostingsCacheMisses'] > 0 AS misses,
    ProfileEvents['TextIndexPostingsCacheHits'] > 0 AS hits
FROM system.query_log
WHERE current_database = currentDatabase()
    AND log_comment IN ('05157_first', '05157_second')
    AND type = 'QueryFinish'
ORDER BY log_comment;

DROP TABLE tab_doc_lengths_cache;
