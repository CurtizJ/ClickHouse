-- Tests that merging text indexes with positions (`support_phrase_search = 1`) keeps phrase
-- search correct when the primary keys of the source parts interleave (ids go round-robin over
-- the three parts), so both the posting streams and the token positions are remapped through
-- `MergedPartOffsets` during the merge.
--
-- All text index parameters are pinned because CI randomizes the corresponding table settings.
-- Rows with the same token set but different token order ('quick brown fox' vs 'brown quick fox')
-- must stay distinguishable after the merge: that only holds if positions were merged correctly.
-- One row per part puts a phrase at token positions 31/32, crossing a roaringish position bucket.

SET use_skip_indexes = 1;
SET use_skip_indexes_on_data_read = 1;
SET query_plan_direct_read_from_text_index = 1;
SET use_query_condition_cache = 0;

DROP TABLE IF EXISTS t_text_merge_pos;

CREATE TABLE t_text_merge_pos
(
    id UInt64,
    s String,
    INDEX idx s TYPE text(
        tokenizer = 'splitByNonAlpha',
        support_phrase_search = 1,
        dictionary_block_size = 4,
        dictionary_block_frontcoding_compression = 1,
        posting_list_block_size = 256,
        posting_list_codec = 'bitpacking')
)
ENGINE = MergeTree ORDER BY id
SETTINGS allow_experimental_text_index_phrase_search = 1,
         max_bytes_to_merge_at_max_space_in_pool = 1; -- no background merges, only OPTIMIZE FINAL below

-- Three parts: part p holds ids p, p + 3, p + 6, ... (number is the row rank inside the part).
INSERT INTO t_text_merge_pos
SELECT number * 3 + 0, multiIf(
    number < 200, 'quick brown fox',
    number < 300, 'brown quick fox',
    number < 350, 'lazy dog sleeps',
    number = 350, concat(repeat('w ', 31), 'alpha beta'),
    number = 351, 'alpha w beta',
    'zpart0 marker')
FROM numbers(400);

INSERT INTO t_text_merge_pos
SELECT number * 3 + 1, multiIf(
    number < 200, 'quick brown fox',
    number < 300, 'brown quick fox',
    number < 350, 'lazy dog sleeps',
    number = 350, concat(repeat('w ', 31), 'alpha beta'),
    number = 351, 'alpha w beta',
    'zpart1 marker')
FROM numbers(400);

INSERT INTO t_text_merge_pos
SELECT number * 3 + 2, multiIf(
    number < 200, 'quick brown fox',
    number < 300, 'brown quick fox',
    number < 350, 'lazy dog sleeps',
    number = 350, concat(repeat('w ', 31), 'alpha beta'),
    number = 351, 'alpha w beta',
    'zpart2 marker')
FROM numbers(400);

SELECT 'parts before merge', count() FROM system.parts WHERE database = currentDatabase() AND table = 't_text_merge_pos' AND active;

SELECT 'phrase search before merge';
SELECT count() FROM t_text_merge_pos WHERE hasPhrase(s, 'quick brown');
SELECT count() FROM t_text_merge_pos WHERE hasPhrase(s, 'brown quick');
SELECT count() FROM t_text_merge_pos WHERE hasPhrase(s, 'alpha beta');

OPTIMIZE TABLE t_text_merge_pos FINAL SETTINGS optimize_throw_if_noop = 1, alter_sync = 2;

SELECT 'parts after merge', count() FROM system.parts WHERE database = currentDatabase() AND table = 't_text_merge_pos' AND active;

SELECT 'merged posting lists';
SELECT token, cardinality, has_embedded_postings, has_raw_postings, has_compressed_postings, num_posting_blocks
FROM mergeTreeTextIndex(currentDatabase(), t_text_merge_pos, idx)
ORDER BY token;

-- For every phrase: (count, sum of matching ids) with the text index vs the same query with
-- skip indexes disabled (brute-force ground truth). The tuples must be identical.
SELECT 'phrase search after merge: with index vs brute force';
SELECT 'quick brown',   (SELECT (count(), sum(id)) FROM t_text_merge_pos WHERE hasPhrase(s, 'quick brown')),   (SELECT (count(), sum(id)) FROM t_text_merge_pos WHERE hasPhrase(s, 'quick brown')   SETTINGS use_skip_indexes = 0);
SELECT 'brown quick',   (SELECT (count(), sum(id)) FROM t_text_merge_pos WHERE hasPhrase(s, 'brown quick')),   (SELECT (count(), sum(id)) FROM t_text_merge_pos WHERE hasPhrase(s, 'brown quick')   SETTINGS use_skip_indexes = 0);
SELECT 'brown fox',     (SELECT (count(), sum(id)) FROM t_text_merge_pos WHERE hasPhrase(s, 'brown fox')),     (SELECT (count(), sum(id)) FROM t_text_merge_pos WHERE hasPhrase(s, 'brown fox')     SETTINGS use_skip_indexes = 0);
SELECT 'quick fox',     (SELECT (count(), sum(id)) FROM t_text_merge_pos WHERE hasPhrase(s, 'quick fox')),     (SELECT (count(), sum(id)) FROM t_text_merge_pos WHERE hasPhrase(s, 'quick fox')     SETTINGS use_skip_indexes = 0);
SELECT 'alpha beta',    (SELECT (count(), sum(id)) FROM t_text_merge_pos WHERE hasPhrase(s, 'alpha beta')),    (SELECT (count(), sum(id)) FROM t_text_merge_pos WHERE hasPhrase(s, 'alpha beta')    SETTINGS use_skip_indexes = 0);
SELECT 'w alpha',       (SELECT (count(), sum(id)) FROM t_text_merge_pos WHERE hasPhrase(s, 'w alpha')),       (SELECT (count(), sum(id)) FROM t_text_merge_pos WHERE hasPhrase(s, 'w alpha')       SETTINGS use_skip_indexes = 0);
SELECT 'w beta',        (SELECT (count(), sum(id)) FROM t_text_merge_pos WHERE hasPhrase(s, 'w beta')),        (SELECT (count(), sum(id)) FROM t_text_merge_pos WHERE hasPhrase(s, 'w beta')        SETTINGS use_skip_indexes = 0);
SELECT 'zpart1 marker', (SELECT (count(), sum(id)) FROM t_text_merge_pos WHERE hasPhrase(s, 'zpart1 marker')), (SELECT (count(), sum(id)) FROM t_text_merge_pos WHERE hasPhrase(s, 'zpart1 marker') SETTINGS use_skip_indexes = 0);
SELECT 'marker zpart1', (SELECT (count(), sum(id)) FROM t_text_merge_pos WHERE hasPhrase(s, 'marker zpart1')), (SELECT (count(), sum(id)) FROM t_text_merge_pos WHERE hasPhrase(s, 'marker zpart1') SETTINGS use_skip_indexes = 0);
SELECT 'fox jumps',     (SELECT (count(), sum(id)) FROM t_text_merge_pos WHERE hasPhrase(s, 'fox jumps')),     (SELECT (count(), sum(id)) FROM t_text_merge_pos WHERE hasPhrase(s, 'fox jumps')     SETTINGS use_skip_indexes = 0);

DROP TABLE t_text_merge_pos;
