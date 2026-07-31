-- Tests merging text index source parts that were written with different posting list codecs,
-- which can happen because `text_index_posting_list_codec` is a mutable table setting
-- (same mechanism as 04338_text_index_codec_setting_change_merge). Here the parts additionally
-- share tokens whose total cardinality crosses the raw-postings threshold (10+10=20 > 12) and
-- the posting block size (300+300=600 > 256), so a roaring-blocks source and a bitpacked source
-- are merged into multi-block destination posting lists. Primary keys interleave (even/odd ids),
-- so the row-id remapping via `MergedPartOffsets` interleaves the two posting streams.
--
-- The index definition deliberately does NOT pin `posting_list_codec`: the codec must come from
-- the mutable table setting, which is pinned in CREATE TABLE (CI randomizes it otherwise).
-- All other text index parameters are pinned in the index definition.

SET use_skip_indexes = 1;
SET use_skip_indexes_on_data_read = 1;
SET query_plan_direct_read_from_text_index = 1;
SET use_query_condition_cache = 0;

SELECT 'none source + bitpacking source merged into bitpacking destination';

DROP TABLE IF EXISTS t_codec_mix;

CREATE TABLE t_codec_mix
(
    id UInt64,
    s String,
    INDEX idx s TYPE text(
        tokenizer = 'splitByNonAlpha',
        dictionary_block_size = 4,
        dictionary_block_frontcoding_compression = 1,
        posting_list_block_size = 256)
)
ENGINE = MergeTree ORDER BY id
SETTINGS text_index_posting_list_codec = 'none',
         max_bytes_to_merge_at_max_space_in_pool = 1; -- no background merges, only OPTIMIZE FINAL below

-- Part 1: even ids, written with the 'none' codec (number is the row rank inside the part).
INSERT INTO t_codec_mix
SELECT number * 2, concat('common p1big',
    if(number < 10, ' shared', ''),
    if(number < 300, ' bulk', ''))
FROM numbers(400);

ALTER TABLE t_codec_mix MODIFY SETTING text_index_posting_list_codec = 'bitpacking';

-- Part 2: odd ids, written with the 'bitpacking' codec.
INSERT INTO t_codec_mix
SELECT number * 2 + 1, concat('common p2big',
    if(number < 10, ' shared', ''),
    if(number < 300, ' bulk', ''))
FROM numbers(400);

-- Prove that the two source parts really use different codecs before the merge:
-- a high-cardinality token unique to the 'none' part has no compressed postings,
-- while one unique to the 'bitpacking' part does.
SELECT 'source codecs differ (p1big, p2big)';
SELECT has_compressed_postings FROM mergeTreeTextIndex(currentDatabase(), t_codec_mix, idx) WHERE token = 'p1big';
SELECT has_compressed_postings FROM mergeTreeTextIndex(currentDatabase(), t_codec_mix, idx) WHERE token = 'p2big';

OPTIMIZE TABLE t_codec_mix FINAL SETTINGS optimize_throw_if_noop = 1, alter_sync = 2;

SELECT 'parts after merge', count() FROM system.parts WHERE database = currentDatabase() AND table = 't_codec_mix' AND active;

SELECT 'merged posting lists';
SELECT token, cardinality, has_embedded_postings, has_raw_postings, has_compressed_postings, num_posting_blocks
FROM mergeTreeTextIndex(currentDatabase(), t_codec_mix, idx)
ORDER BY token;

SELECT 'search results after merge: with index vs brute force';
SELECT 'common', (SELECT (count(), sum(id)) FROM t_codec_mix WHERE hasToken(s, 'common')), (SELECT (count(), sum(id)) FROM t_codec_mix WHERE hasToken(s, 'common') SETTINGS use_skip_indexes = 0);
SELECT 'shared', (SELECT (count(), sum(id)) FROM t_codec_mix WHERE hasToken(s, 'shared')), (SELECT (count(), sum(id)) FROM t_codec_mix WHERE hasToken(s, 'shared') SETTINGS use_skip_indexes = 0);
SELECT 'bulk',   (SELECT (count(), sum(id)) FROM t_codec_mix WHERE hasToken(s, 'bulk')),   (SELECT (count(), sum(id)) FROM t_codec_mix WHERE hasToken(s, 'bulk')   SETTINGS use_skip_indexes = 0);
SELECT 'p1big',  (SELECT (count(), sum(id)) FROM t_codec_mix WHERE hasToken(s, 'p1big')),  (SELECT (count(), sum(id)) FROM t_codec_mix WHERE hasToken(s, 'p1big')  SETTINGS use_skip_indexes = 0);
SELECT 'p2big',  (SELECT (count(), sum(id)) FROM t_codec_mix WHERE hasToken(s, 'p2big')),  (SELECT (count(), sum(id)) FROM t_codec_mix WHERE hasToken(s, 'p2big')  SETTINGS use_skip_indexes = 0);

DROP TABLE t_codec_mix;

SELECT 'bitpacking source + none source merged into none destination';

DROP TABLE IF EXISTS t_codec_mix;

CREATE TABLE t_codec_mix
(
    id UInt64,
    s String,
    INDEX idx s TYPE text(
        tokenizer = 'splitByNonAlpha',
        dictionary_block_size = 4,
        dictionary_block_frontcoding_compression = 1,
        posting_list_block_size = 256)
)
ENGINE = MergeTree ORDER BY id
SETTINGS text_index_posting_list_codec = 'bitpacking',
         max_bytes_to_merge_at_max_space_in_pool = 1;

-- Part 1: even ids, written with the 'bitpacking' codec.
INSERT INTO t_codec_mix
SELECT number * 2, concat('common p1big',
    if(number < 10, ' shared', ''),
    if(number < 300, ' bulk', ''))
FROM numbers(400);

ALTER TABLE t_codec_mix MODIFY SETTING text_index_posting_list_codec = 'none';

-- Part 2: odd ids, written with the 'none' codec.
INSERT INTO t_codec_mix
SELECT number * 2 + 1, concat('common p2big',
    if(number < 10, ' shared', ''),
    if(number < 300, ' bulk', ''))
FROM numbers(400);

SELECT 'source codecs differ (p1big, p2big)';
SELECT has_compressed_postings FROM mergeTreeTextIndex(currentDatabase(), t_codec_mix, idx) WHERE token = 'p1big';
SELECT has_compressed_postings FROM mergeTreeTextIndex(currentDatabase(), t_codec_mix, idx) WHERE token = 'p2big';

OPTIMIZE TABLE t_codec_mix FINAL SETTINGS optimize_throw_if_noop = 1, alter_sync = 2;

SELECT 'parts after merge', count() FROM system.parts WHERE database = currentDatabase() AND table = 't_codec_mix' AND active;

SELECT 'merged posting lists';
SELECT token, cardinality, has_embedded_postings, has_raw_postings, has_compressed_postings, num_posting_blocks
FROM mergeTreeTextIndex(currentDatabase(), t_codec_mix, idx)
ORDER BY token;

SELECT 'search results after merge: with index vs brute force';
SELECT 'common', (SELECT (count(), sum(id)) FROM t_codec_mix WHERE hasToken(s, 'common')), (SELECT (count(), sum(id)) FROM t_codec_mix WHERE hasToken(s, 'common') SETTINGS use_skip_indexes = 0);
SELECT 'shared', (SELECT (count(), sum(id)) FROM t_codec_mix WHERE hasToken(s, 'shared')), (SELECT (count(), sum(id)) FROM t_codec_mix WHERE hasToken(s, 'shared') SETTINGS use_skip_indexes = 0);
SELECT 'bulk',   (SELECT (count(), sum(id)) FROM t_codec_mix WHERE hasToken(s, 'bulk')),   (SELECT (count(), sum(id)) FROM t_codec_mix WHERE hasToken(s, 'bulk')   SETTINGS use_skip_indexes = 0);
SELECT 'p1big',  (SELECT (count(), sum(id)) FROM t_codec_mix WHERE hasToken(s, 'p1big')),  (SELECT (count(), sum(id)) FROM t_codec_mix WHERE hasToken(s, 'p1big')  SETTINGS use_skip_indexes = 0);
SELECT 'p2big',  (SELECT (count(), sum(id)) FROM t_codec_mix WHERE hasToken(s, 'p2big')),  (SELECT (count(), sum(id)) FROM t_codec_mix WHERE hasToken(s, 'p2big')  SETTINGS use_skip_indexes = 0);

DROP TABLE t_codec_mix;
