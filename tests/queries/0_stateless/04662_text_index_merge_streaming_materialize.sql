-- Tests a text index merge where some source parts have no materialized index
-- (inserted with `materialize_skip_indexes_on_insert = 0`): the merge builds temporary index
-- segments for those parts and merges them with the already-materialized index of the other part
-- (`MergeTextIndexesTask`). Primary keys of the parts interleave (ids go round-robin), so the
-- row-id remapping via `MergedPartOffsets` genuinely interleaves the posting streams.
--
-- All text index parameters are pinned because CI randomizes the corresponding table settings.
-- `add_minmax_index_for_numeric_columns = 0` is pinned so `secondary_indices_marks_bytes = 0`
-- really means "no materialized text index".
--
-- Token totals cover both sides of the embedded (6) and raw (12) posting-list thresholds
-- with sources mixed between temporary segments and the materialized index:
--   emb (3+3), cmp (6+7), mixm (300+3+10), bignoidx/bigidx (400, unique to one part),
--   noidx0/idx1/noidx2 (5, unique to one part), common (400+400+400).

SET use_skip_indexes = 1;
SET use_skip_indexes_on_data_read = 1;
SET query_plan_direct_read_from_text_index = 1;
SET use_query_condition_cache = 0;

DROP TABLE IF EXISTS t_text_merge_mat;

CREATE TABLE t_text_merge_mat
(
    id UInt64,
    s String,
    INDEX idx s TYPE text(
        tokenizer = 'splitByNonAlpha',
        dictionary_block_size = 4,
        dictionary_block_frontcoding_compression = 1,
        posting_list_block_size = 256,
        posting_list_codec = 'bitpacking')
)
ENGINE = MergeTree ORDER BY id
SETTINGS add_minmax_index_for_numeric_columns = 0,
         max_bytes_to_merge_at_max_space_in_pool = 1; -- no background merges, only OPTIMIZE FINAL below

-- Part 1: ids 0, 3, 6, ... without a materialized index.
SET materialize_skip_indexes_on_insert = 0;
INSERT INTO t_text_merge_mat
SELECT number * 3, concat('common bignoidx',
    if(number < 5, ' noidx0', ''),
    if(number < 3, ' emb', ''),
    if(number < 6, ' cmp', ''),
    if(number < 300, ' mixm', ''))
FROM numbers(400);

-- Part 2: ids 1, 4, 7, ... with a materialized index.
SET materialize_skip_indexes_on_insert = 1;
INSERT INTO t_text_merge_mat
SELECT number * 3 + 1, concat('common bigidx',
    if(number < 5, ' idx1', ''),
    if(number < 3, ' emb', ''),
    if(number < 7, ' cmp', ''),
    if(number < 3, ' mixm', ''))
FROM numbers(400);

-- Part 3: ids 2, 5, 8, ... without a materialized index.
SET materialize_skip_indexes_on_insert = 0;
INSERT INTO t_text_merge_mat
SELECT number * 3 + 2, concat('common',
    if(number < 5, ' noidx2', ''),
    if(number < 10, ' mixm', ''))
FROM numbers(400);

SELECT 'parts before merge', count() FROM system.parts WHERE database = currentDatabase() AND table = 't_text_merge_mat' AND active;
SELECT 'parts without materialized index before merge', count() FROM system.parts WHERE database = currentDatabase() AND table = 't_text_merge_mat' AND active AND secondary_indices_marks_bytes = 0;

SELECT 'search before merge (index partially materialized)';
SELECT count() FROM t_text_merge_mat WHERE hasToken(s, 'common');
SELECT count() FROM t_text_merge_mat WHERE hasToken(s, 'mixm');

OPTIMIZE TABLE t_text_merge_mat FINAL SETTINGS optimize_throw_if_noop = 1, alter_sync = 2;

SELECT 'parts after merge', count() FROM system.parts WHERE database = currentDatabase() AND table = 't_text_merge_mat' AND active;
SELECT 'parts without materialized index after merge', count() FROM system.parts WHERE database = currentDatabase() AND table = 't_text_merge_mat' AND active AND secondary_indices_marks_bytes = 0;

SELECT 'merged posting lists';
SELECT token, cardinality, has_embedded_postings, has_raw_postings, has_compressed_postings, num_posting_blocks
FROM mergeTreeTextIndex(currentDatabase(), t_text_merge_mat, idx)
ORDER BY token;

-- For every token: (count, sum of matching ids) with the text index vs the same query with
-- skip indexes disabled (brute-force ground truth). The tuples must be identical.
SELECT 'search results after merge: with index vs brute force';
SELECT 'common',   (SELECT (count(), sum(id)) FROM t_text_merge_mat WHERE hasToken(s, 'common')),   (SELECT (count(), sum(id)) FROM t_text_merge_mat WHERE hasToken(s, 'common')   SETTINGS use_skip_indexes = 0);
SELECT 'bignoidx', (SELECT (count(), sum(id)) FROM t_text_merge_mat WHERE hasToken(s, 'bignoidx')), (SELECT (count(), sum(id)) FROM t_text_merge_mat WHERE hasToken(s, 'bignoidx')  SETTINGS use_skip_indexes = 0);
SELECT 'bigidx',   (SELECT (count(), sum(id)) FROM t_text_merge_mat WHERE hasToken(s, 'bigidx')),   (SELECT (count(), sum(id)) FROM t_text_merge_mat WHERE hasToken(s, 'bigidx')   SETTINGS use_skip_indexes = 0);
SELECT 'noidx0',   (SELECT (count(), sum(id)) FROM t_text_merge_mat WHERE hasToken(s, 'noidx0')),   (SELECT (count(), sum(id)) FROM t_text_merge_mat WHERE hasToken(s, 'noidx0')   SETTINGS use_skip_indexes = 0);
SELECT 'idx1',     (SELECT (count(), sum(id)) FROM t_text_merge_mat WHERE hasToken(s, 'idx1')),     (SELECT (count(), sum(id)) FROM t_text_merge_mat WHERE hasToken(s, 'idx1')     SETTINGS use_skip_indexes = 0);
SELECT 'noidx2',   (SELECT (count(), sum(id)) FROM t_text_merge_mat WHERE hasToken(s, 'noidx2')),   (SELECT (count(), sum(id)) FROM t_text_merge_mat WHERE hasToken(s, 'noidx2')   SETTINGS use_skip_indexes = 0);
SELECT 'emb',      (SELECT (count(), sum(id)) FROM t_text_merge_mat WHERE hasToken(s, 'emb')),      (SELECT (count(), sum(id)) FROM t_text_merge_mat WHERE hasToken(s, 'emb')      SETTINGS use_skip_indexes = 0);
SELECT 'cmp',      (SELECT (count(), sum(id)) FROM t_text_merge_mat WHERE hasToken(s, 'cmp')),      (SELECT (count(), sum(id)) FROM t_text_merge_mat WHERE hasToken(s, 'cmp')      SETTINGS use_skip_indexes = 0);
SELECT 'mixm',     (SELECT (count(), sum(id)) FROM t_text_merge_mat WHERE hasToken(s, 'mixm')),     (SELECT (count(), sum(id)) FROM t_text_merge_mat WHERE hasToken(s, 'mixm')     SETTINGS use_skip_indexes = 0);

SELECT 'multi-token search functions';
SELECT count() FROM t_text_merge_mat WHERE hasAllTokens(s, ['common', 'idx1']);
SELECT count() FROM t_text_merge_mat WHERE hasAnyTokens(s, ['noidx0', 'noidx2']);
SELECT count() FROM t_text_merge_mat WHERE hasAllTokens(s, ['bignoidx', 'mixm']);

DROP TABLE t_text_merge_mat;
