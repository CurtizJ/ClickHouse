-- Tags: no-parallel-replicas

-- Tests that the text index analysis decodes only the packed blocks of a posting list that some query can use:
-- the blocks with rows readable after the primary key analysis, and for `All` queries the blocks with a row
-- of the intersection folded so far. The other blocks cannot change the result, so they are skipped.

SET enable_analyzer = 1;
SET use_skip_indexes = 1;
SET use_query_condition_cache = 0;
SET query_plan_optimize_count_from_text_index = 0;
SET use_text_index_postings_cache = 0;
-- Analyze the index once per part, at the planning stage, so that the counts of the packed blocks below are exact.
SET use_skip_indexes_on_data_read = 0;
SET query_plan_direct_read_from_text_index = 0;

DROP TABLE IF EXISTS tab_pfor;
DROP TABLE IF EXISTS tab_bitpacking;

-- One part with one posting list segment per token, so the packed blocks of 'common' cover 128 consecutive rows each:
--   'common' -- every row, 200000 postings in 1563 blocks;
--   'rare'   -- 20 rows, one every 10000 rows starting from 7, each in a different block of 'common';
--   'rare2'  -- 20 rows, one every 10000 rows starting from 5007;
--   'tiny'   -- 3 rows, embedded into the dictionary.
CREATE TABLE tab_pfor
(
    id UInt64,
    s String,
    INDEX idx_s s TYPE text(tokenizer = splitByNonAlpha, posting_list_codec = 'pfor', posting_list_block_size = 1048576)
)
ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 8192, index_granularity_bytes = '10Mi';

CREATE TABLE tab_bitpacking
(
    id UInt64,
    s String,
    INDEX idx_s s TYPE text(tokenizer = splitByNonAlpha, posting_list_codec = 'bitpacking', posting_list_block_size = 1048576)
)
ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 8192, index_granularity_bytes = '10Mi';

INSERT INTO tab_pfor SELECT
    number,
    arrayStringConcat(arrayFilter(x -> x != '', [
        'common',
        if(number % 10000 = 7, 'rare', ''),
        if(number % 10000 = 5007, 'rare2', ''),
        if(number IN (7, 100007, 150007), 'tiny', '')]), ' ')
FROM numbers(200000);

INSERT INTO tab_bitpacking SELECT * FROM tab_pfor;

OPTIMIZE TABLE tab_pfor FINAL;
OPTIMIZE TABLE tab_bitpacking FINAL;

-- 'rare' is read first, then only the 20 blocks of 'common' with a row of 'rare' are decoded.
SELECT count() FROM tab_pfor WHERE hasAllTokens(s, ['common', 'rare']) SETTINGS log_comment = 'skip_blocks_pfor_all_rare';
-- The intersection starts from the embedded postings of 'tiny': 3 blocks of 'common'.
SELECT count() FROM tab_pfor WHERE hasAllTokens(s, ['common', 'tiny']) SETTINGS log_comment = 'skip_blocks_pfor_all_tiny';
-- The primary key leaves the rows from 98304 readable: the block of 'rare' is decoded, 'rare' keeps 10 rows,
-- then 10 blocks of 'common' are decoded.
SELECT count() FROM tab_pfor WHERE id >= 100000 AND hasAllTokens(s, ['common', 'rare']) SETTINGS log_comment = 'skip_blocks_pfor_all_pk';
-- An `Any` query takes every readable row: the primary key leaves the rows up to 24575 readable,
-- so the block of 'rare' and the 192 blocks of 'common' inside them are decoded.
SELECT count() FROM tab_pfor WHERE id < 20000 AND hasAnyTokens(s, ['common', 'rare']) SETTINGS log_comment = 'skip_blocks_pfor_any_pk';
-- 'common' is used by an `Any` query too, so its posting list is read in full.
SELECT count() FROM tab_pfor WHERE hasAllTokens(s, ['common', 'rare']) AND hasAnyTokens(s, ['common', 'tiny']) SETTINGS log_comment = 'skip_blocks_pfor_mixed';
-- Two `All` queries share 'common': the blocks with a row of either intersection are decoded.
SELECT count() FROM tab_pfor WHERE hasAllTokens(s, ['common', 'rare']) OR hasAllTokens(s, ['common', 'rare2']) SETTINGS log_comment = 'skip_blocks_pfor_two_all';

SELECT count() FROM tab_bitpacking WHERE hasAllTokens(s, ['common', 'rare']) SETTINGS log_comment = 'skip_blocks_bitpacking_all_rare';
SELECT count() FROM tab_bitpacking WHERE id >= 100000 AND hasAllTokens(s, ['common', 'rare']) SETTINGS log_comment = 'skip_blocks_bitpacking_all_pk';
SELECT count() FROM tab_bitpacking WHERE id < 20000 AND hasAnyTokens(s, ['common', 'rare']) SETTINGS log_comment = 'skip_blocks_bitpacking_any_pk';

SYSTEM FLUSH LOGS query_log;

SELECT
    log_comment,
    ProfileEvents['TextIndexAnalysisPackedBlocksDecoded'] AS blocks_decoded,
    ProfileEvents['TextIndexAnalysisPackedBlocksSkipped'] AS blocks_skipped
FROM system.query_log
WHERE event_date >= yesterday() AND event_time >= now() - 600
    AND current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment LIKE 'skip\\_blocks\\_%'
ORDER BY log_comment;

SELECT '-- the results do not depend on the index and on the way it is applied';

SELECT id FROM tab_pfor WHERE id >= 100000 AND hasAllTokens(s, ['common', 'rare']) ORDER BY id;
SELECT id FROM tab_pfor WHERE id >= 100000 AND hasAllTokens(s, ['common', 'rare']) ORDER BY id SETTINGS use_skip_indexes = 0;

SELECT
    count() FILTER (WHERE hasAllTokens(s, ['common', 'rare'])),
    count() FILTER (WHERE hasAllTokens(s, ['common', 'tiny'])),
    count() FILTER (WHERE id >= 100000 AND hasAllTokens(s, ['common', 'rare'])),
    count() FILTER (WHERE id < 20000 AND hasAnyTokens(s, ['common', 'rare'])),
    count() FILTER (WHERE hasAllTokens(s, ['common', 'rare']) OR hasAllTokens(s, ['common', 'rare2']))
FROM tab_pfor SETTINGS use_skip_indexes = 0;

SELECT count() FROM tab_pfor WHERE id >= 100000 AND hasAllTokens(s, ['common', 'rare'])
SETTINGS use_skip_indexes_on_data_read = 1, query_plan_direct_read_from_text_index = 1, text_index_posting_list_apply_mode = 'lazy';
SELECT count() FROM tab_pfor WHERE id >= 100000 AND hasAllTokens(s, ['common', 'rare'])
SETTINGS use_skip_indexes_on_data_read = 1, query_plan_direct_read_from_text_index = 1, text_index_posting_list_apply_mode = 'materialize';
SELECT count() FROM tab_pfor WHERE hasAllTokens(s, ['common', 'rare']) OR hasAllTokens(s, ['common', 'rare2'])
SETTINGS query_plan_direct_read_from_text_index = 1;
SELECT count() FROM tab_pfor WHERE hasAllTokens(s, ['common', 'rare']) SETTINGS query_plan_optimize_count_from_text_index = 1;
SELECT count() FROM tab_pfor WHERE hasAnyTokens(s, ['rare', 'rare2']) SETTINGS query_plan_optimize_count_from_text_index = 1;

DROP TABLE tab_pfor;
DROP TABLE tab_bitpacking;
