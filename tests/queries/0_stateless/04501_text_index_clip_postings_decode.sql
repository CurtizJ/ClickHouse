-- Tags: no-parallel-replicas

-- Tests that reading token postings during text index analysis is clipped to the current
-- readable rows: with the bitpacking codec only the packed blocks intersecting the readable
-- row ranges (and, for a token used by a single 'All'-mode query, the query's current row
-- range) are decompressed. Query results must not change.

SET enable_analyzer = 1;
SET use_skip_indexes = 1;
-- The query condition cache remembers which granules failed WHERE from previous runs and would
-- mask the actual index behavior, so disable it for this test.
SET use_query_condition_cache = 0;

DROP TABLE IF EXISTS tab;

CREATE TABLE tab
(
    id UInt64,
    s String,
    INDEX idx_s s TYPE text(tokenizer = splitByNonAlpha)
)
ENGINE = MergeTree ORDER BY id
-- Pin the posting list layout so each token is a single compressed segment with a per-block index;
-- otherwise a randomized 'text_index_posting_list_block_size' or codec could change the layout.
SETTINGS index_granularity = 8192, text_index_posting_list_block_size = 1048576, text_index_posting_list_codec = 'bitpacking';

-- 'common' occurs in every row, 'left' only in rows [0, 500000), 'right' only in rows [400000, 1000000).
INSERT INTO tab SELECT number, if(number < 400000, 'common left', if(number < 500000, 'common left right', 'common right')) FROM numbers(1000000);
OPTIMIZE TABLE tab FINAL;

-- Correctness with clipped decoding. The primary key condition clips the readable rows for
-- 'common'; the intersection of the token ranges clips 'left' and 'right' for the 'All'-mode
-- hasAllTokens queries; the whole-part query is not clipped at all.
SELECT count() FROM tab WHERE id < 100000 AND hasToken(s, 'common');
SELECT count() FROM tab WHERE id >= 300000 AND id < 350000 AND hasToken(s, 'common');
SELECT count() FROM tab WHERE hasAllTokens(s, ['left', 'right']);
SELECT count() FROM tab WHERE id < 450000 AND hasAllTokens(s, ['left', 'right']);
SELECT count() FROM tab WHERE hasToken(s, 'common');

-- Equivalence: the same predicates must return the same counts with skip indexes disabled.
-- This proves the clipped decoding never loses matching rows.
SELECT count() FROM tab WHERE id < 100000 AND hasToken(s, 'common') SETTINGS use_skip_indexes = 0;
SELECT count() FROM tab WHERE id >= 300000 AND id < 350000 AND hasToken(s, 'common') SETTINGS use_skip_indexes = 0;
SELECT count() FROM tab WHERE hasAllTokens(s, ['left', 'right']) SETTINGS use_skip_indexes = 0;
SELECT count() FROM tab WHERE id < 450000 AND hasAllTokens(s, ['left', 'right']) SETTINGS use_skip_indexes = 0;
SELECT count() FROM tab WHERE hasToken(s, 'common') SETTINGS use_skip_indexes = 0;

SYSTEM FLUSH LOGS query_log;

-- Packed blocks are skipped exactly for the queries where the readable rows or the single
-- 'All'-query row range covers the posting lists partially: the four clipped queries above skip
-- blocks, the whole-part query does not. Expected: 1, 1, 1, 1, 0 (in execution order).
SELECT ProfileEvents['TextIndexPostingsBlocksSkipped'] > 0
FROM system.query_log
WHERE event_date >= yesterday() AND event_time >= now() - 600
  AND current_database = currentDatabase() AND type = 'QueryFinish'
  AND query LIKE '%SELECT count() FROM tab WHERE%'
  AND query NOT LIKE '%use_skip_indexes = 0%' AND query NOT LIKE '%query_log%'
ORDER BY event_time_microseconds;

DROP TABLE tab;
