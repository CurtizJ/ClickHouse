-- Tags: no-parallel-replicas
-- no-parallel-replicas: the test checks the exact query plan via EXPLAIN.

SET enable_analyzer = 1;
SET allow_experimental_bm25_score_column = 1;
SET query_plan_direct_read_from_text_index = 1;
SET use_skip_indexes_on_data_read = 1;
SET use_top_k_dynamic_filtering = 1;

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

-- The scoring tokens are rare (a few percent of the rows), so the text index reader
-- emits the `_bm25_score` column in the sparse representation. The filler rows keep
-- the estimated score density below the sparse threshold.
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
    EXPLAIN actions = 1 SELECT id, round(_bm25_score, 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft consensus') ORDER BY _bm25_score DESC, id LIMIT 5
)
WHERE explain LIKE '%__topKFilter(_bm25_score)%';

SELECT 'direct read applied', count() > 0 FROM
(
    EXPLAIN actions = 1 SELECT id, round(_bm25_score, 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft consensus') ORDER BY _bm25_score DESC, id LIMIT 5
)
WHERE explain LIKE '%__text_index_idx_body_hasAnyTokens%';

SELECT 'single token desc';
SELECT id, round(_bm25_score, 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft') ORDER BY _bm25_score DESC, id LIMIT 5;

SELECT 'single token desc reference';
SELECT id, round(_bm25_score, 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft') ORDER BY _bm25_score DESC, id LIMIT 5
SETTINGS use_top_k_dynamic_filtering = 0, use_skip_indexes_for_top_k = 0;

SELECT 'union desc';
SELECT id, round(_bm25_score, 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft consensus') ORDER BY _bm25_score DESC, id LIMIT 5;

SELECT 'union desc reference';
SELECT id, round(_bm25_score, 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft consensus') ORDER BY _bm25_score DESC, id LIMIT 5
SETTINGS use_top_k_dynamic_filtering = 0, use_skip_indexes_for_top_k = 0;

-- With ascending order the zero default passes the threshold, so the topK filter
-- cannot stay sparse and expands to a full column.
SELECT 'union asc';
SELECT id, round(_bm25_score, 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft consensus') ORDER BY _bm25_score ASC, id LIMIT 5;

SELECT 'union asc reference';
SELECT id, round(_bm25_score, 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft consensus') ORDER BY _bm25_score ASC, id LIMIT 5
SETTINGS use_top_k_dynamic_filtering = 0, use_skip_indexes_for_top_k = 0;

SELECT 'intersection desc';
SELECT id, round(_bm25_score, 4) FROM tab_bm25_sparse WHERE hasAllTokens(body, ['raft', 'consensus']) ORDER BY _bm25_score DESC, id LIMIT 5;

SELECT 'intersection desc reference';
SELECT id, round(_bm25_score, 4) FROM tab_bm25_sparse WHERE hasAllTokens(body, ['raft', 'consensus']) ORDER BY _bm25_score DESC, id LIMIT 5
SETTINGS use_top_k_dynamic_filtering = 0, use_skip_indexes_for_top_k = 0;

-- Order-independent checksums over all scored rows (not only the top of the heap).
SELECT 'union checksum';
SELECT count(), sum(toDecimal64(_bm25_score, 3)), round(max(_bm25_score), 4) FROM tab_bm25_sparse WHERE hasAnyTokens(body, 'raft consensus');

SELECT 'intersection checksum';
SELECT count(), sum(toDecimal64(_bm25_score, 3)), round(max(_bm25_score), 4) FROM tab_bm25_sparse WHERE hasAllTokens(body, ['raft', 'consensus']);

DROP TABLE tab_bm25_sparse;
