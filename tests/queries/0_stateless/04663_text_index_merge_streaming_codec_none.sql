-- Tests merging text indexes into a destination with `posting_list_codec = 'none'` (posting lists
-- are stored as roaring blocks instead of bitpacked segments). Same data layout as the bitpacking
-- test 04660_text_index_merge_streaming_postings: three parts whose primary keys interleave
-- round-robin, so the row-id remapping via `MergedPartOffsets` interleaves the posting streams.
--
-- All text index parameters are pinned because CI randomizes the corresponding table settings.
-- `dictionary_block_size = 1` puts every token into its own dictionary block, so during the merge
-- every source dictionary block gets exhausted and replaced while its last token is still pending.
--
-- Token totals cover both sides of the embedded (6) and raw (12) posting-list thresholds
-- (see 04660 for the per-token breakdown).

SET use_skip_indexes = 1;
SET use_skip_indexes_on_data_read = 1;
SET query_plan_direct_read_from_text_index = 1;
SET use_query_condition_cache = 0;

DROP TABLE IF EXISTS t_text_merge_none;

CREATE TABLE t_text_merge_none
(
    id UInt64,
    s String,
    INDEX idx s TYPE text(
        tokenizer = 'splitByNonAlpha',
        dictionary_block_size = 1,
        dictionary_block_frontcoding_compression = 1,
        posting_list_block_size = 256,
        posting_list_codec = 'none')
)
ENGINE = MergeTree ORDER BY id
SETTINGS max_bytes_to_merge_at_max_space_in_pool = 1; -- no background merges, only OPTIMIZE FINAL below

-- Part 1: ids 0, 3, 6, ... (number is the row rank inside the part).
INSERT INTO t_text_merge_none
SELECT number * 3, concat('common big0',
    if(number < 5, ' only0', ''),
    if(number < 3, ' emb33 raw34 mix3a', ''),
    if(number < 6, ' raw66 cmp67', ''),
    if(number < 86, ' edge256 edge257', ''))
FROM numbers(400);

-- Part 2: ids 1, 4, 7, ...
INSERT INTO t_text_merge_none
SELECT number * 3 + 1, concat('common',
    if(number < 5, ' only1', ''),
    if(number < 3, ' emb33', ''),
    if(number < 4, ' raw34', ''),
    if(number < 6, ' raw66', ''),
    if(number < 7, ' cmp67', ''),
    if(number < 300, ' mix3a mixraw', ''),
    if(number < 85, ' edge256', ''),
    if(number < 86, ' edge257', ''))
FROM numbers(400);

-- Part 3: ids 2, 5, 8, ...
INSERT INTO t_text_merge_none
SELECT number * 3 + 2, concat('common',
    if(number < 5, ' only2', ''),
    if(number < 10, ' mixraw', ''),
    if(number < 85, ' edge256 edge257', ''))
FROM numbers(400);

SELECT 'parts before merge', count() FROM system.parts WHERE database = currentDatabase() AND table = 't_text_merge_none' AND active;

-- The sources are roaring posting lists (no compressed postings anywhere).
SELECT 'source posting lists per part';
SELECT token, cardinality, has_embedded_postings, has_raw_postings, has_compressed_postings, num_posting_blocks
FROM mergeTreeTextIndex(currentDatabase(), t_text_merge_none, idx)
WHERE token IN ('common', 'big0', 'only0', 'only1', 'only2', 'emb33', 'raw34', 'raw66', 'cmp67', 'mix3a', 'mixraw', 'edge256', 'edge257')
ORDER BY part_name, token;

OPTIMIZE TABLE t_text_merge_none FINAL SETTINGS optimize_throw_if_noop = 1, alter_sync = 2;

SELECT 'parts after merge', count() FROM system.parts WHERE database = currentDatabase() AND table = 't_text_merge_none' AND active;

-- The destination codec is 'none', so no merged posting list may have compressed postings.
SELECT 'merged posting lists';
SELECT token, cardinality, has_embedded_postings, has_raw_postings, has_compressed_postings, num_posting_blocks
FROM mergeTreeTextIndex(currentDatabase(), t_text_merge_none, idx)
ORDER BY token;

-- For every token: (count, sum of matching ids) with the text index vs the same query with
-- skip indexes disabled (brute-force ground truth). The tuples must be identical.
SELECT 'search results after merge: with index vs brute force';
SELECT 'common',  (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'common')),  (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'common')  SETTINGS use_skip_indexes = 0);
SELECT 'big0',    (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'big0')),    (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'big0')    SETTINGS use_skip_indexes = 0);
SELECT 'only0',   (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'only0')),   (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'only0')   SETTINGS use_skip_indexes = 0);
SELECT 'only1',   (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'only1')),   (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'only1')   SETTINGS use_skip_indexes = 0);
SELECT 'only2',   (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'only2')),   (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'only2')   SETTINGS use_skip_indexes = 0);
SELECT 'emb33',   (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'emb33')),   (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'emb33')   SETTINGS use_skip_indexes = 0);
SELECT 'raw34',   (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'raw34')),   (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'raw34')   SETTINGS use_skip_indexes = 0);
SELECT 'raw66',   (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'raw66')),   (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'raw66')   SETTINGS use_skip_indexes = 0);
SELECT 'cmp67',   (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'cmp67')),   (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'cmp67')   SETTINGS use_skip_indexes = 0);
SELECT 'mix3a',   (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'mix3a')),   (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'mix3a')   SETTINGS use_skip_indexes = 0);
SELECT 'mixraw',  (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'mixraw')),  (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'mixraw')  SETTINGS use_skip_indexes = 0);
SELECT 'edge256', (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'edge256')), (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'edge256') SETTINGS use_skip_indexes = 0);
SELECT 'edge257', (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'edge257')), (SELECT (count(), sum(id)) FROM t_text_merge_none WHERE hasToken(s, 'edge257') SETTINGS use_skip_indexes = 0);

DROP TABLE t_text_merge_none;
