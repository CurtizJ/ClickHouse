-- Tags: no-parallel-replicas

-- Tests that the analysis of a text index granule folds the posting lists of rare tokens without decoding
-- the packed blocks no query can use. For an `All` query the rows folded so far bound the blocks decoded
-- for the next token, for an `Any` query the rows readable after the primary key analysis do. The counts
-- below hold for both block codecs, the uncompressed codec has no blocks to skip and serves as a reference.

SET enable_analyzer = 1;
SET use_skip_indexes = 1;
SET use_query_condition_cache = 0;
SET query_plan_optimize_count_from_text_index = 0;
-- Keep the counts below independent of what other queries have already cached.
SET use_text_index_postings_cache = 0;

DROP TABLE IF EXISTS tab_bitpacking;
DROP TABLE IF EXISTS tab_pfor;
DROP TABLE IF EXISTS tab_none;

-- One part, one posting list segment per token, so the packed blocks of 'common' cover 128 consecutive rows each:
--   'common' -- every row, 1000000 postings in 7813 blocks;
--   'rare'   -- 20 rows, one every 50000 rows starting from 7, each in a different block of 'common';
--   'tiny'   -- 3 rows, embedded into the dictionary;
--   'raw'    -- 9 rows, stored as raw values without compression;
--   'edge'   -- rows 3 and 999999: its row range covers the part, but it shares no row with 'rare'.
CREATE TABLE tab_bitpacking
(
    id UInt64,
    s String,
    INDEX idx_s s TYPE text(tokenizer = splitByNonAlpha)
)
ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 8192, text_index_posting_list_block_size = 1048576, text_index_posting_list_codec = 'bitpacking';

CREATE TABLE tab_pfor AS tab_bitpacking
ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 8192, text_index_posting_list_block_size = 1048576, text_index_posting_list_codec = 'pfor';

CREATE TABLE tab_none AS tab_bitpacking
ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 8192, text_index_posting_list_block_size = 1048576, text_index_posting_list_codec = 'none';

INSERT INTO tab_bitpacking SELECT
    number,
    arrayStringConcat(arrayFilter(x -> x != '', [
        'common',
        if(number % 50000 = 7, 'rare', ''),
        if(number IN (7, 100007, 500007), 'tiny', ''),
        if(number IN (7, 50007, 100007, 300007, 500007, 700007, 900007, 950007, 999999), 'raw', ''),
        if(number IN (3, 999999), 'edge', '')]), ' ')
FROM numbers(1000000);

INSERT INTO tab_pfor SELECT * FROM tab_bitpacking;
INSERT INTO tab_none SELECT * FROM tab_bitpacking;

OPTIMIZE TABLE tab_bitpacking FINAL;
OPTIMIZE TABLE tab_pfor FINAL;
OPTIMIZE TABLE tab_none FINAL;

SELECT '-- bitpacking';

-- 'rare' is folded first (1 block), then only the 20 blocks of 'common' with a row of 'rare' are decoded.
SELECT count() FROM tab_bitpacking WHERE hasAllTokens(s, ['common', 'rare']) SETTINGS log_comment = '05230_case_bitpacking_all_rare';
-- The intersection is seeded from the embedded postings of 'tiny': 3 blocks of 'common'.
SELECT count() FROM tab_bitpacking WHERE hasAllTokens(s, ['common', 'tiny']) SETTINGS log_comment = '05230_case_bitpacking_all_tiny';
-- The intersection is seeded from the raw postings of 'raw': 9 blocks of 'common'.
SELECT count() FROM tab_bitpacking WHERE hasAllTokens(s, ['common', 'raw']) SETTINGS log_comment = '05230_case_bitpacking_all_raw';
-- 'edge' is embedded and folded first; the posting list of 'rare' has no row of the intersection and is not read at all.
SELECT count() FROM tab_bitpacking WHERE hasAllTokens(s, ['rare', 'edge']) SETTINGS log_comment = '05230_case_bitpacking_all_edge';
-- The primary key leaves the rows from 499712 readable: 'rare' keeps 10 rows, then 10 blocks of 'common' are decoded.
SELECT count() FROM tab_bitpacking WHERE id >= 500000 AND hasAllTokens(s, ['common', 'rare']) SETTINGS log_comment = '05230_case_bitpacking_all_pk';
-- 'Any' query: the primary key leaves the rows up to 106495 readable, only the 832 blocks of 'common' inside them are decoded.
SELECT count() FROM tab_bitpacking WHERE id < 100000 AND hasAnyTokens(s, ['common', 'rare']) SETTINGS log_comment = '05230_case_bitpacking_any_pk';
-- The same token in an `All` and an `Any` query: the `Any` query needs every block.
SELECT count() FROM tab_bitpacking WHERE hasAllTokens(s, ['common', 'rare']) AND hasAnyTokens(s, ['common', 'tiny']) SETTINGS log_comment = '05230_case_bitpacking_mixed';
-- Materialized posting lists and the count optimization consume the folded intersection too.
SELECT count() FROM tab_bitpacking WHERE hasAllTokens(s, ['common', 'rare']) SETTINGS text_index_posting_list_apply_mode = 'materialize';
SELECT count() FROM tab_bitpacking WHERE id >= 500000 AND hasAllTokens(s, ['common', 'rare']) SETTINGS text_index_posting_list_apply_mode = 'materialize';
SELECT count() FROM tab_bitpacking WHERE hasAllTokens(s, ['common', 'rare']) SETTINGS query_plan_optimize_count_from_text_index = 1;
SELECT count() FROM tab_bitpacking WHERE hasAnyTokens(s, ['rare', 'tiny']) SETTINGS query_plan_optimize_count_from_text_index = 1;
-- A pattern query folds the matched tokens as an `Any` query.
SELECT count() FROM tab_bitpacking WHERE id < 100000 AND s LIKE '%rare%';
SELECT count() FROM tab_bitpacking WHERE hasAllTokens(s, ['common', 'rare']) AND s LIKE '%tiny%';
-- Rows of the folded intersection.
SELECT id FROM tab_bitpacking WHERE id >= 500000 AND hasAllTokens(s, ['common', 'rare']) ORDER BY id;

SELECT '-- pfor';

SELECT count() FROM tab_pfor WHERE hasAllTokens(s, ['common', 'rare']) SETTINGS log_comment = '05230_case_pfor_all_rare';
SELECT count() FROM tab_pfor WHERE hasAllTokens(s, ['common', 'tiny']) SETTINGS log_comment = '05230_case_pfor_all_tiny';
SELECT count() FROM tab_pfor WHERE hasAllTokens(s, ['common', 'raw']) SETTINGS log_comment = '05230_case_pfor_all_raw';
SELECT count() FROM tab_pfor WHERE hasAllTokens(s, ['rare', 'edge']) SETTINGS log_comment = '05230_case_pfor_all_edge';
SELECT count() FROM tab_pfor WHERE id >= 500000 AND hasAllTokens(s, ['common', 'rare']) SETTINGS log_comment = '05230_case_pfor_all_pk';
SELECT count() FROM tab_pfor WHERE id < 100000 AND hasAnyTokens(s, ['common', 'rare']) SETTINGS log_comment = '05230_case_pfor_any_pk';
SELECT count() FROM tab_pfor WHERE hasAllTokens(s, ['common', 'rare']) AND hasAnyTokens(s, ['common', 'tiny']) SETTINGS log_comment = '05230_case_pfor_mixed';
SELECT count() FROM tab_pfor WHERE hasAllTokens(s, ['common', 'rare']) SETTINGS text_index_posting_list_apply_mode = 'materialize';
SELECT count() FROM tab_pfor WHERE id >= 500000 AND hasAllTokens(s, ['common', 'rare']) SETTINGS text_index_posting_list_apply_mode = 'materialize';
SELECT count() FROM tab_pfor WHERE hasAllTokens(s, ['common', 'rare']) SETTINGS query_plan_optimize_count_from_text_index = 1;
SELECT count() FROM tab_pfor WHERE hasAnyTokens(s, ['rare', 'tiny']) SETTINGS query_plan_optimize_count_from_text_index = 1;
SELECT count() FROM tab_pfor WHERE id < 100000 AND s LIKE '%rare%';
SELECT count() FROM tab_pfor WHERE hasAllTokens(s, ['common', 'rare']) AND s LIKE '%tiny%';
SELECT id FROM tab_pfor WHERE id >= 500000 AND hasAllTokens(s, ['common', 'rare']) ORDER BY id;

SELECT '-- none';

SELECT count() FROM tab_none WHERE hasAllTokens(s, ['common', 'rare']) SETTINGS log_comment = '05230_case_none_all_rare';
SELECT count() FROM tab_none WHERE hasAllTokens(s, ['common', 'tiny']) SETTINGS log_comment = '05230_case_none_all_tiny';
SELECT count() FROM tab_none WHERE hasAllTokens(s, ['common', 'raw']) SETTINGS log_comment = '05230_case_none_all_raw';
SELECT count() FROM tab_none WHERE hasAllTokens(s, ['rare', 'edge']) SETTINGS log_comment = '05230_case_none_all_edge';
SELECT count() FROM tab_none WHERE id >= 500000 AND hasAllTokens(s, ['common', 'rare']) SETTINGS log_comment = '05230_case_none_all_pk';
SELECT count() FROM tab_none WHERE id < 100000 AND hasAnyTokens(s, ['common', 'rare']) SETTINGS log_comment = '05230_case_none_any_pk';
SELECT count() FROM tab_none WHERE hasAllTokens(s, ['common', 'rare']) AND hasAnyTokens(s, ['common', 'tiny']) SETTINGS log_comment = '05230_case_none_mixed';
SELECT count() FROM tab_none WHERE hasAllTokens(s, ['common', 'rare']) SETTINGS text_index_posting_list_apply_mode = 'materialize';
SELECT count() FROM tab_none WHERE id >= 500000 AND hasAllTokens(s, ['common', 'rare']) SETTINGS text_index_posting_list_apply_mode = 'materialize';
SELECT count() FROM tab_none WHERE hasAllTokens(s, ['common', 'rare']) SETTINGS query_plan_optimize_count_from_text_index = 1;
SELECT count() FROM tab_none WHERE hasAnyTokens(s, ['rare', 'tiny']) SETTINGS query_plan_optimize_count_from_text_index = 1;
SELECT count() FROM tab_none WHERE id < 100000 AND s LIKE '%rare%';
SELECT count() FROM tab_none WHERE hasAllTokens(s, ['common', 'rare']) AND s LIKE '%tiny%';
SELECT id FROM tab_none WHERE id >= 500000 AND hasAllTokens(s, ['common', 'rare']) ORDER BY id;

SELECT '-- without the index';

SELECT count() FROM tab_bitpacking WHERE hasAllTokens(s, ['common', 'rare']) SETTINGS use_skip_indexes = 0;
SELECT count() FROM tab_bitpacking WHERE hasAllTokens(s, ['common', 'tiny']) SETTINGS use_skip_indexes = 0;
SELECT count() FROM tab_bitpacking WHERE hasAllTokens(s, ['common', 'raw']) SETTINGS use_skip_indexes = 0;
SELECT count() FROM tab_bitpacking WHERE hasAllTokens(s, ['rare', 'edge']) SETTINGS use_skip_indexes = 0;
SELECT count() FROM tab_bitpacking WHERE id >= 500000 AND hasAllTokens(s, ['common', 'rare']) SETTINGS use_skip_indexes = 0;
SELECT count() FROM tab_bitpacking WHERE id < 100000 AND hasAnyTokens(s, ['common', 'rare']) SETTINGS use_skip_indexes = 0;
SELECT count() FROM tab_bitpacking WHERE hasAllTokens(s, ['common', 'rare']) AND hasAnyTokens(s, ['common', 'tiny']) SETTINGS use_skip_indexes = 0;
SELECT count() FROM tab_bitpacking WHERE hasAnyTokens(s, ['rare', 'tiny']) SETTINGS use_skip_indexes = 0;
SELECT count() FROM tab_bitpacking WHERE id < 100000 AND s LIKE '%rare%' SETTINGS use_skip_indexes = 0;
SELECT count() FROM tab_bitpacking WHERE hasAllTokens(s, ['common', 'rare']) AND s LIKE '%tiny%' SETTINGS use_skip_indexes = 0;
SELECT id FROM tab_bitpacking WHERE id >= 500000 AND hasAllTokens(s, ['common', 'rare']) ORDER BY id SETTINGS use_skip_indexes = 0;

SYSTEM FLUSH LOGS query_log;

SELECT '-- packed blocks decoded and skipped, posting list segments skipped, posting lists read';

SELECT
    log_comment,
    ProfileEvents['TextIndexAnalyzePostingsBlocksDecoded'] AS blocks_decoded,
    ProfileEvents['TextIndexAnalyzePostingsBlocksSkipped'] AS blocks_skipped,
    ProfileEvents['TextIndexAnalyzePostingsSegmentsSkipped'] AS segments_skipped,
    ProfileEvents['TextIndexReadPostings'] AS postings_read
FROM system.query_log
WHERE event_date >= yesterday() AND event_time >= now() - 600
  AND current_database = currentDatabase() AND type = 'QueryFinish'
  AND log_comment LIKE '05230_case_%'
ORDER BY log_comment;

DROP TABLE tab_bitpacking;
DROP TABLE tab_pfor;
DROP TABLE tab_none;
