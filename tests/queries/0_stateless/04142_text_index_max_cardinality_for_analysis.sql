-- Tags: no-parallel-replicas

-- Tests that the `text_index_max_cardinality_for_analysis` setting controls
-- which tokens have their full posting list eagerly loaded during the first
-- stage of text index granule analysis. Tokens with cardinality at or below
-- the threshold get their posting list loaded across all blocks, enabling
-- tighter granule pruning. Tokens above the threshold are conservatively
-- assumed to be present in every granule whose block range intersects.

DROP TABLE IF EXISTS t_text_idx_card;

CREATE TABLE t_text_idx_card
(
    id UInt32,
    message String,
    INDEX idx(message) TYPE text(tokenizer = splitByNonAlpha, posting_list_block_size = 4, posting_list_codec = 'none') GRANULARITY 1
)
ENGINE = MergeTree
ORDER BY id
SETTINGS index_granularity = 50;

-- Token 'foo' lives at rows 0, 100, 200, ..., 1200 (13 occurrences = cardinality 13).
-- With `posting_list_block_size = 4`, this becomes a multi-block token (4 blocks).
-- Block ranges are wide (e.g., block 0 covers row range 0..200) and thus intersect
-- many granules; without loading the posting list, those granules cannot be pruned.
-- Filler 'bar' rows ensure other granules exist that should be pruned by the dictionary.
INSERT INTO t_text_idx_card
SELECT number, if(number % 100 = 0 AND number <= 1200, 'foo', 'bar')
FROM numbers(1300);

SELECT 'Sanity: foo cardinality and block count';
SELECT token, cardinality, num_posting_blocks
FROM mergeTreeTextIndex(currentDatabase(), 't_text_idx_card', 'idx')
WHERE token = 'foo';

SELECT 'Result correctness across thresholds';
SELECT count() FROM t_text_idx_card WHERE hasToken(message, 'foo') SETTINGS text_index_max_cardinality_for_analysis = 0;
SELECT count() FROM t_text_idx_card WHERE hasToken(message, 'foo') SETTINGS text_index_max_cardinality_for_analysis = 12;
SELECT count() FROM t_text_idx_card WHERE hasToken(message, 'foo') SETTINGS text_index_max_cardinality_for_analysis = 13;
SELECT count() FROM t_text_idx_card WHERE hasToken(message, 'foo') SETTINGS text_index_max_cardinality_for_analysis = 1000000;
SELECT count() FROM t_text_idx_card WHERE hasToken(message, 'foo') SETTINGS use_skip_indexes = 0;

SELECT 'Granule pruning - threshold below cardinality (postings not loaded)';
SELECT trimLeft(explain) AS explain FROM (
    EXPLAIN indexes = 1
    SELECT count() FROM t_text_idx_card WHERE hasToken(message, 'foo')
    SETTINGS text_index_max_cardinality_for_analysis = 0
) WHERE explain LIKE '%Granules:%';

SELECT 'Granule pruning - threshold at cardinality (postings loaded)';
SELECT trimLeft(explain) AS explain FROM (
    EXPLAIN indexes = 1
    SELECT count() FROM t_text_idx_card WHERE hasToken(message, 'foo')
    SETTINGS text_index_max_cardinality_for_analysis = 13
) WHERE explain LIKE '%Granules:%';

SELECT 'Granule pruning - threshold well above cardinality (postings loaded)';
SELECT trimLeft(explain) AS explain FROM (
    EXPLAIN indexes = 1
    SELECT count() FROM t_text_idx_card WHERE hasToken(message, 'foo')
    SETTINGS text_index_max_cardinality_for_analysis = 1000000
) WHERE explain LIKE '%Granules:%';

DROP TABLE t_text_idx_card;
