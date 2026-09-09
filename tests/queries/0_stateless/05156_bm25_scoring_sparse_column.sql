-- Tags: no-parallel-replicas
-- no-parallel-replicas: the test checks the exact query plan via EXPLAIN and per-query ProfileEvents.

-- The text index reader fills the `bm25()` score column as a sparse column when the posting lists
-- bound the scored rows to a small share of the part, and `__topKFilter` keeps such a filter sparse.
-- Every sparse result is followed by the same query with sparse columns disabled
-- (`text_index_ratio_of_defaults_for_sparse_columns = 1.0`): the two must agree exactly.

SET enable_analyzer = 1;
SET allow_experimental_bm25_scoring = 1;
SET query_plan_direct_read_from_text_index = 1;
SET use_skip_indexes_on_data_read = 1;
SET use_top_k_dynamic_filtering = 1;
SET log_queries = 1;
SET log_comment = '05156_bm25_scoring_sparse_column';

DROP TABLE IF EXISTS tab_bm25_sparse;

CREATE TABLE tab_bm25_sparse
(
    id UInt32,
    body String,
    INDEX idx_body(body) TYPE text(tokenizer = 'splitByNonAlpha', posting_list_codec = 'bitpacking', enable_scoring = 1) GRANULARITY 1
)
ENGINE = MergeTree
ORDER BY id
SETTINGS index_granularity = 16, allow_experimental_text_index_scoring = 1;

SYSTEM STOP MERGES tab_bm25_sparse;

-- The scoring tokens are rare (a few percent of the rows), so the score column is sparse.
-- The filler rows keep the estimated share of scored rows below the sparse threshold.
INSERT INTO tab_bm25_sparse
SELECT
    number,
    concat('filler text entry ', toString(number), if(number % 61 = 0, ' raft', ''), if(number % 97 = 0, ' consensus', ''))
FROM numbers(1000);

-- The second part also gets sparse scores and contains rows matching both tokens.
INSERT INTO tab_bm25_sparse
SELECT
    number + 10000,
    concat('other filler ', toString(number), if(number % 53 = 0, ' raft consensus', ''))
FROM numbers(500);

SELECT 'top-K dynamic filter applied', count() > 0 FROM
(
    EXPLAIN actions = 1 SELECT id, round(bm25(), 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft consensus') ORDER BY bm25() DESC, id LIMIT 5
)
WHERE explain LIKE '%__topKFilter(__text_index_idx_body_bm25_%';

SELECT 'direct read applied', count() > 0 FROM
(
    EXPLAIN actions = 1 SELECT id, round(bm25(), 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft consensus') ORDER BY bm25() DESC, id LIMIT 5
)
WHERE explain LIKE '%__text_index_idx_body_hasAnyTokens%';

SELECT 'single token desc';
SELECT id, round(bm25(), 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft') ORDER BY bm25() DESC, id LIMIT 5;

SELECT 'single token desc reference';
SELECT id, round(bm25(), 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft') ORDER BY bm25() DESC, id LIMIT 5
SETTINGS text_index_ratio_of_defaults_for_sparse_columns = 1.0;

SELECT 'union desc';
SELECT id, round(bm25(), 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft consensus') ORDER BY bm25() DESC, id LIMIT 5;

SELECT 'union desc reference';
SELECT id, round(bm25(), 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft consensus') ORDER BY bm25() DESC, id LIMIT 5
SETTINGS text_index_ratio_of_defaults_for_sparse_columns = 1.0;

-- With the ascending order the zero default passes the threshold, so the top-k filter cannot stay
-- sparse and expands to a full column.
SELECT 'union asc';
SELECT id, round(bm25(), 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft consensus') ORDER BY bm25() ASC, id LIMIT 5;

SELECT 'union asc reference';
SELECT id, round(bm25(), 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft consensus') ORDER BY bm25() ASC, id LIMIT 5
SETTINGS text_index_ratio_of_defaults_for_sparse_columns = 1.0;

SELECT 'intersection desc';
SELECT id, round(bm25(), 4) FROM tab_bm25_sparse WHERE hasAllTokens(body, ['raft', 'consensus']) ORDER BY bm25() DESC, id LIMIT 5;

SELECT 'intersection desc reference';
SELECT id, round(bm25(), 4) FROM tab_bm25_sparse WHERE hasAllTokens(body, ['raft', 'consensus']) ORDER BY bm25() DESC, id LIMIT 5
SETTINGS text_index_ratio_of_defaults_for_sparse_columns = 1.0;

-- The pruning of the marks and windows zero-fills a sparse score column as well.
SELECT 'pruning with sparse scores';
SELECT id, round(bm25(), 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft consensus') ORDER BY bm25() DESC, id LIMIT 3
SETTINGS text_index_bm25_pruning = 1;

SELECT 'pruning with sparse scores reference';
SELECT id, round(bm25(), 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft consensus') ORDER BY bm25() DESC, id LIMIT 3
SETTINGS text_index_bm25_pruning = 0, text_index_ratio_of_defaults_for_sparse_columns = 1.0;

-- Order-independent checksums over all scored rows, not only the top of the heap.
SELECT 'union checksum';
SELECT count(), sum(toDecimal64(bm25(), 3)), round(max(bm25()), 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft consensus');

SELECT 'union checksum reference';
SELECT count(), sum(toDecimal64(bm25(), 3)), round(max(bm25()), 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft consensus')
SETTINGS text_index_ratio_of_defaults_for_sparse_columns = 1.0;

SELECT 'intersection checksum';
SELECT count(), sum(toDecimal64(bm25(), 3)), round(max(bm25()), 4) FROM tab_bm25_sparse WHERE hasAllTokens(body, ['raft', 'consensus']);

SELECT 'intersection checksum reference';
SELECT count(), sum(toDecimal64(bm25(), 3)), round(max(bm25()), 4) FROM tab_bm25_sparse WHERE hasAllTokens(body, ['raft', 'consensus'])
SETTINGS text_index_ratio_of_defaults_for_sparse_columns = 1.0;

-- The columns are sparse only below the threshold: `raft` is in a few rows, `filler` in every row.
SELECT 'probe rare', count(), round(sum(bm25()), 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft');
SELECT 'probe common', count(), round(sum(bm25()), 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'filler');

SELECT 'sparse virtual columns per query';
SYSTEM FLUSH LOGS query_log;

SELECT
    extract(query, 'hasAnyTokens\\(body, ''(\\w+)''\\)') AS token,
    ProfileEvents['TextIndexSparseVirtualColumns'] > 0 AS any_sparse_column
FROM system.query_log
WHERE current_database = currentDatabase()
    AND log_comment = '05156_bm25_scoring_sparse_column'
    AND type = 'QueryFinish'
    AND query LIKE '%''probe %'
ORDER BY event_time_microseconds;

DROP TABLE tab_bm25_sparse;
