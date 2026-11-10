-- Tags: no-parallel-replicas

-- Tests that the text index analysis approximates a dense posting list by the row spans of its packed blocks
-- for `Any` queries and single-token queries, instead of decoding it, and that the approximation selects exactly
-- the same granules as the decoded posting list: a mark holds a row of the token if and only if it holds a row
-- of the approximation. The readers read the approximated posting lists themselves.

SET enable_analyzer = 1;
SET use_skip_indexes = 1;
SET use_query_condition_cache = 0;
SET use_text_index_postings_cache = 0;
-- Analyze the index once per part, at the planning stage, so that the counts of the approximated posting lists are exact.
SET use_skip_indexes_on_data_read = 0;
SET query_plan_direct_read_from_text_index = 0;
SET query_plan_optimize_count_from_text_index = 0;
SET text_index_posting_list_apply_mode = 'lazy';

DROP TABLE IF EXISTS tab;

-- One part of 25 marks with one posting list segment per token:
--   'dense'  -- every second row of [0, 50000) and every row of [150000, 160000): marks 0-6, 18, 19;
--   'rare'   -- rows 100003, 120003, ..., 180003: marks 12, 14, 17, 19, 21;
--   'sparse' -- every 997th row: its packed blocks span many marks, so it is decoded.
CREATE TABLE tab
(
    id UInt64,
    s String,
    INDEX idx_s s TYPE text(tokenizer = splitByNonAlpha, posting_list_codec = 'pfor', posting_list_block_size = 1048576)
)
ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 8192, index_granularity_bytes = '10Mi', min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0;

INSERT INTO tab SELECT
    number,
    arrayStringConcat(arrayFilter(x -> x != '', [
        if((number < 50000 AND number % 2 = 0) OR (number >= 150000 AND number < 160000), 'dense', ''),
        if(number >= 100000 AND number % 20000 = 3, 'rare', ''),
        if(number % 997 = 0, 'sparse', ''),
        'filler']), ' ')
FROM numbers(200000);

OPTIMIZE TABLE tab FINAL;

SELECT '-- the approximation selects the same granules as the decoded posting lists';

SELECT trim(explain) FROM (EXPLAIN indexes = 1 SELECT count() FROM tab WHERE hasAnyTokens(s, ['dense', 'rare'])) WHERE explain LIKE '%Granules%';
SELECT trim(explain) FROM (EXPLAIN indexes = 1 SELECT count() FROM tab WHERE hasAnyTokens(s, ['dense', 'rare']) SETTINGS text_index_posting_list_apply_mode = 'materialize') WHERE explain LIKE '%Granules%';
SELECT trim(explain) FROM (EXPLAIN indexes = 1 SELECT count() FROM tab WHERE hasToken(s, 'dense')) WHERE explain LIKE '%Granules%';
SELECT trim(explain) FROM (EXPLAIN indexes = 1 SELECT count() FROM tab WHERE hasToken(s, 'dense') SETTINGS text_index_posting_list_apply_mode = 'materialize') WHERE explain LIKE '%Granules%';
SELECT trim(explain) FROM (EXPLAIN indexes = 1 SELECT count() FROM tab WHERE id >= 100000 AND hasAnyTokens(s, ['dense', 'rare'])) WHERE explain LIKE '%Granules%';
SELECT trim(explain) FROM (EXPLAIN indexes = 1 SELECT count() FROM tab WHERE id >= 100000 AND hasAnyTokens(s, ['dense', 'rare']) SETTINGS text_index_posting_list_apply_mode = 'materialize') WHERE explain LIKE '%Granules%';

SELECT '-- which posting lists are approximated';

-- 'dense' is approximated, 'rare' is decoded.
SELECT count() FROM tab WHERE hasAnyTokens(s, ['dense', 'rare']) SETTINGS log_comment = 'approximate_any';
-- A single-token query takes an approximation as well.
SELECT count() FROM tab WHERE hasToken(s, 'dense') SETTINGS log_comment = 'approximate_single';
-- An intersection of approximations is not exact at the granularity of marks, so nothing is approximated.
SELECT count() FROM tab WHERE hasAllTokens(s, ['dense', 'rare']) SETTINGS log_comment = 'approximate_all';
-- The packed blocks of 'sparse' span whole marks, so it is decoded.
SELECT count() FROM tab WHERE hasAnyTokens(s, ['sparse', 'rare']) SETTINGS log_comment = 'approximate_sparse';
-- The materialize mode reads no posting lists in the readers, so nothing is approximated.
SELECT count() FROM tab WHERE hasAnyTokens(s, ['dense', 'rare']) SETTINGS log_comment = 'approximate_materialize', text_index_posting_list_apply_mode = 'materialize';

SYSTEM FLUSH LOGS query_log;

SELECT log_comment, ProfileEvents['TextIndexAnalysisApproximatedPostings'] AS approximated
FROM system.query_log
WHERE event_date >= yesterday() AND event_time >= now() - 600
    AND current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment LIKE 'approximate\\_%'
ORDER BY log_comment;

SELECT '-- the results do not depend on the index and on the way it is applied';

SELECT
    count() FILTER (WHERE hasAnyTokens(s, ['dense', 'rare'])),
    count() FILTER (WHERE hasToken(s, 'dense')),
    count() FILTER (WHERE id >= 100000 AND hasAnyTokens(s, ['dense', 'rare'])),
    count() FILTER (WHERE hasAnyTokens(s, ['sparse', 'dense'])),
    sum(id) FILTER (WHERE hasAnyTokens(s, ['dense', 'rare']))
FROM tab SETTINGS use_skip_indexes = 0;

SELECT
    (SELECT count() FROM tab WHERE hasAnyTokens(s, ['dense', 'rare'])),
    (SELECT count() FROM tab WHERE hasToken(s, 'dense')),
    (SELECT count() FROM tab WHERE id >= 100000 AND hasAnyTokens(s, ['dense', 'rare'])),
    (SELECT count() FROM tab WHERE hasAnyTokens(s, ['sparse', 'dense'])),
    (SELECT sum(id) FROM tab WHERE hasAnyTokens(s, ['dense', 'rare']))
SETTINGS query_plan_direct_read_from_text_index = 1;

SELECT
    (SELECT count() FROM tab WHERE hasAnyTokens(s, ['dense', 'rare'])),
    (SELECT count() FROM tab WHERE hasToken(s, 'dense')),
    (SELECT count() FROM tab WHERE id >= 100000 AND hasAnyTokens(s, ['dense', 'rare'])),
    (SELECT count() FROM tab WHERE hasAnyTokens(s, ['sparse', 'dense'])),
    (SELECT sum(id) FROM tab WHERE hasAnyTokens(s, ['dense', 'rare']))
SETTINGS query_plan_direct_read_from_text_index = 1, use_skip_indexes_on_data_read = 1;

SELECT
    (SELECT count() FROM tab WHERE hasAnyTokens(s, ['dense', 'rare'])),
    (SELECT count() FROM tab WHERE hasToken(s, 'dense')),
    (SELECT count() FROM tab WHERE id >= 100000 AND hasAnyTokens(s, ['dense', 'rare'])),
    (SELECT count() FROM tab WHERE hasAnyTokens(s, ['sparse', 'dense']))
SETTINGS query_plan_optimize_count_from_text_index = 1;

DROP TABLE tab;
