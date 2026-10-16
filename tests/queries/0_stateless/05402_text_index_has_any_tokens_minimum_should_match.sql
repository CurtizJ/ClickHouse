-- Tags: no-parallel-replicas
-- `hasAnyTokens(input, needles, minimum_should_match[, tokenizer])` matches rows with at least `minimum_should_match`
-- distinct tokens of `needles`. The text index answers it in the lazy and materialize modes, directly and with the
-- count from the index, and the results must match the row-level function.

SET enable_analyzer = 1;
SET use_skip_indexes = 1;
SET query_plan_direct_read_from_text_index = 1;
SET optimize_trivial_count_query = 1;
SET make_distributed_plan = 0;
SET serialize_query_plan = 0;
SET max_rows_to_group_by = 0;
-- The granules pruned by the query condition cache entries of the previous queries disable the count from the index.
SET use_query_condition_cache = 0;
-- `max_rows_to_read` with `read_overflow_mode = 'throw'` disables `use_skip_indexes_on_data_read`.
SET max_rows_to_read = 0;

SELECT 'function';
SELECT hasAnyTokens('a b c', ['a', 'b', 'x'], 2);
SELECT hasAnyTokens('a b c', ['a', 'x', 'y'], 2);
SELECT hasAnyTokens('a b c', ['a', 'b', 'c'], 3);
SELECT hasAnyTokens('a b c', ['a', 'b'], 3);
SELECT hasAnyTokens('a a a', ['a', 'b'], 2);
SELECT hasAnyTokens('a b c', ['a', 'a', 'b'], 2);
SELECT hasAnyTokens('a b c', 'a b x', 2);
SELECT hasAnyTokens('a b c', ['a', 'x'], 1);
SELECT hasAnyTokens('a,b;c', ['a', 'b'], toInt8(2), 'splitByNonAlpha');
SELECT hasAnyTokens(['x y', 'z'], ['x y', 'z', 'w'], 2, 'array');
SELECT hasAnyTokens(['a b', 'c d'], ['a', 'd', 'x'], 2);
SELECT hasAnyTokens(toNullable('a b'), ['a', 'b'], 2), hasAnyTokens(CAST(NULL, 'Nullable(String)'), ['a', 'b'], 2);
SELECT hasAnyTokens(materialize('a b c'), ['a', 'b', 'x'], 2) FROM numbers(2);
SELECT hasAnyTokens('a b c', ['a', 'b'], 0); -- { serverError BAD_ARGUMENTS }
SELECT hasAnyTokens('a b c', ['a', 'b'], -1); -- { serverError BAD_ARGUMENTS }
SELECT hasAnyTokens('a b c', ['a', 'b'], materialize(2)); -- { serverError ILLEGAL_COLUMN }

DROP TABLE IF EXISTS tab;

CREATE TABLE tab
(
    id UInt64,
    text String,
    INDEX idx text TYPE text(tokenizer = splitByNonAlpha, posting_list_codec = 'pfor', posting_list_block_size = 1000)
)
ENGINE = MergeTree ORDER BY id;

-- `two`, `three` and `five` have compressed posting lists of many blocks, `rare` a small uncompressed one.
INSERT INTO tab SELECT number, concat(
    'w',
    if(number % 2 = 0, ' two', ''),
    if(number % 3 = 0, ' three', ''),
    if(number % 5 = 0, ' five', ''),
    if(number % 1000 = 8, ' rare', ''))
FROM numbers(100000);

SELECT count() > 0 FROM (EXPLAIN actions = 1 SELECT id FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 2))
WHERE explain LIKE '%__text_index_idx_hasAnyTokens%';
SELECT count() > 0 FROM (EXPLAIN SELECT count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 2))
WHERE explain LIKE '%ReadFromTextIndexCount%';

SELECT '[two, three, five], 2';
SELECT 'lazy', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 2) SETTINGS text_index_posting_list_apply_mode = 'lazy', query_plan_optimize_count_from_text_index = 0;
SELECT 'materialize', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 2) SETTINGS text_index_posting_list_apply_mode = 'materialize', query_plan_optimize_count_from_text_index = 0;
SELECT 'count lazy', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 2) SETTINGS text_index_posting_list_apply_mode = 'lazy', query_plan_optimize_count_from_text_index = 1;
SELECT 'count materialize', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 2) SETTINGS text_index_posting_list_apply_mode = 'materialize', query_plan_optimize_count_from_text_index = 1;
SELECT 'scan', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 2) SETTINGS use_skip_indexes = 0, query_plan_direct_read_from_text_index = 0;

SELECT '[two, three, five], 3';
SELECT 'lazy', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 3) SETTINGS text_index_posting_list_apply_mode = 'lazy', query_plan_optimize_count_from_text_index = 0;
SELECT 'materialize', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 3) SETTINGS text_index_posting_list_apply_mode = 'materialize', query_plan_optimize_count_from_text_index = 0;
SELECT 'count lazy', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 3) SETTINGS text_index_posting_list_apply_mode = 'lazy', query_plan_optimize_count_from_text_index = 1;
SELECT 'count materialize', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 3) SETTINGS text_index_posting_list_apply_mode = 'materialize', query_plan_optimize_count_from_text_index = 1;
SELECT 'scan', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 3) SETTINGS use_skip_indexes = 0, query_plan_direct_read_from_text_index = 0;

SELECT '[two, three, five], 1';
SELECT 'lazy', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 1) SETTINGS text_index_posting_list_apply_mode = 'lazy', query_plan_optimize_count_from_text_index = 0;
SELECT 'materialize', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 1) SETTINGS text_index_posting_list_apply_mode = 'materialize', query_plan_optimize_count_from_text_index = 0;
SELECT 'count lazy', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 1) SETTINGS text_index_posting_list_apply_mode = 'lazy', query_plan_optimize_count_from_text_index = 1;
SELECT 'count materialize', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 1) SETTINGS text_index_posting_list_apply_mode = 'materialize', query_plan_optimize_count_from_text_index = 1;
SELECT 'scan', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 1) SETTINGS use_skip_indexes = 0, query_plan_direct_read_from_text_index = 0;

SELECT '[two, rare], 2';
SELECT 'lazy', count() FROM tab WHERE hasAnyTokens(text, ['two', 'rare'], 2) SETTINGS text_index_posting_list_apply_mode = 'lazy', query_plan_optimize_count_from_text_index = 0;
SELECT 'materialize', count() FROM tab WHERE hasAnyTokens(text, ['two', 'rare'], 2) SETTINGS text_index_posting_list_apply_mode = 'materialize', query_plan_optimize_count_from_text_index = 0;
SELECT 'count lazy', count() FROM tab WHERE hasAnyTokens(text, ['two', 'rare'], 2) SETTINGS text_index_posting_list_apply_mode = 'lazy', query_plan_optimize_count_from_text_index = 1;
SELECT 'count materialize', count() FROM tab WHERE hasAnyTokens(text, ['two', 'rare'], 2) SETTINGS text_index_posting_list_apply_mode = 'materialize', query_plan_optimize_count_from_text_index = 1;
SELECT 'scan', count() FROM tab WHERE hasAnyTokens(text, ['two', 'rare'], 2) SETTINGS use_skip_indexes = 0, query_plan_direct_read_from_text_index = 0;

SELECT '[rare, three, five], 2';
SELECT 'lazy', count() FROM tab WHERE hasAnyTokens(text, ['rare', 'three', 'five'], 2) SETTINGS text_index_posting_list_apply_mode = 'lazy', query_plan_optimize_count_from_text_index = 0;
SELECT 'materialize', count() FROM tab WHERE hasAnyTokens(text, ['rare', 'three', 'five'], 2) SETTINGS text_index_posting_list_apply_mode = 'materialize', query_plan_optimize_count_from_text_index = 0;
SELECT 'count lazy', count() FROM tab WHERE hasAnyTokens(text, ['rare', 'three', 'five'], 2) SETTINGS text_index_posting_list_apply_mode = 'lazy', query_plan_optimize_count_from_text_index = 1;
SELECT 'count materialize', count() FROM tab WHERE hasAnyTokens(text, ['rare', 'three', 'five'], 2) SETTINGS text_index_posting_list_apply_mode = 'materialize', query_plan_optimize_count_from_text_index = 1;
SELECT 'scan', count() FROM tab WHERE hasAnyTokens(text, ['rare', 'three', 'five'], 2) SETTINGS use_skip_indexes = 0, query_plan_direct_read_from_text_index = 0;

SELECT '[two, missing, three], 2';
SELECT 'lazy', count() FROM tab WHERE hasAnyTokens(text, ['two', 'missing', 'three'], 2) SETTINGS text_index_posting_list_apply_mode = 'lazy', query_plan_optimize_count_from_text_index = 0;
SELECT 'materialize', count() FROM tab WHERE hasAnyTokens(text, ['two', 'missing', 'three'], 2) SETTINGS text_index_posting_list_apply_mode = 'materialize', query_plan_optimize_count_from_text_index = 0;
SELECT 'count lazy', count() FROM tab WHERE hasAnyTokens(text, ['two', 'missing', 'three'], 2) SETTINGS text_index_posting_list_apply_mode = 'lazy', query_plan_optimize_count_from_text_index = 1;
SELECT 'count materialize', count() FROM tab WHERE hasAnyTokens(text, ['two', 'missing', 'three'], 2) SETTINGS text_index_posting_list_apply_mode = 'materialize', query_plan_optimize_count_from_text_index = 1;
SELECT 'scan', count() FROM tab WHERE hasAnyTokens(text, ['two', 'missing', 'three'], 2) SETTINGS use_skip_indexes = 0, query_plan_direct_read_from_text_index = 0;

SELECT '[two, missing, three], 3';
SELECT 'lazy', count() FROM tab WHERE hasAnyTokens(text, ['two', 'missing', 'three'], 3) SETTINGS text_index_posting_list_apply_mode = 'lazy', query_plan_optimize_count_from_text_index = 0;
SELECT 'materialize', count() FROM tab WHERE hasAnyTokens(text, ['two', 'missing', 'three'], 3) SETTINGS text_index_posting_list_apply_mode = 'materialize', query_plan_optimize_count_from_text_index = 0;
SELECT 'count lazy', count() FROM tab WHERE hasAnyTokens(text, ['two', 'missing', 'three'], 3) SETTINGS text_index_posting_list_apply_mode = 'lazy', query_plan_optimize_count_from_text_index = 1;
SELECT 'count materialize', count() FROM tab WHERE hasAnyTokens(text, ['two', 'missing', 'three'], 3) SETTINGS text_index_posting_list_apply_mode = 'materialize', query_plan_optimize_count_from_text_index = 1;
SELECT 'scan', count() FROM tab WHERE hasAnyTokens(text, ['two', 'missing', 'three'], 3) SETTINGS use_skip_indexes = 0, query_plan_direct_read_from_text_index = 0;

SELECT '[two, three], 3';
SELECT 'lazy', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three'], 3) SETTINGS text_index_posting_list_apply_mode = 'lazy', query_plan_optimize_count_from_text_index = 0;
SELECT 'materialize', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three'], 3) SETTINGS text_index_posting_list_apply_mode = 'materialize', query_plan_optimize_count_from_text_index = 0;
SELECT 'count lazy', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three'], 3) SETTINGS text_index_posting_list_apply_mode = 'lazy', query_plan_optimize_count_from_text_index = 1;
SELECT 'count materialize', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three'], 3) SETTINGS text_index_posting_list_apply_mode = 'materialize', query_plan_optimize_count_from_text_index = 1;
SELECT 'scan', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three'], 3) SETTINGS use_skip_indexes = 0, query_plan_direct_read_from_text_index = 0;

SELECT 'two three five, 2';
SELECT 'lazy', count() FROM tab WHERE hasAnyTokens(text, 'two three five', 2) SETTINGS text_index_posting_list_apply_mode = 'lazy', query_plan_optimize_count_from_text_index = 0;
SELECT 'materialize', count() FROM tab WHERE hasAnyTokens(text, 'two three five', 2) SETTINGS text_index_posting_list_apply_mode = 'materialize', query_plan_optimize_count_from_text_index = 0;
SELECT 'count lazy', count() FROM tab WHERE hasAnyTokens(text, 'two three five', 2) SETTINGS text_index_posting_list_apply_mode = 'lazy', query_plan_optimize_count_from_text_index = 1;
SELECT 'count materialize', count() FROM tab WHERE hasAnyTokens(text, 'two three five', 2) SETTINGS text_index_posting_list_apply_mode = 'materialize', query_plan_optimize_count_from_text_index = 1;
SELECT 'scan', count() FROM tab WHERE hasAnyTokens(text, 'two three five', 2) SETTINGS use_skip_indexes = 0, query_plan_direct_read_from_text_index = 0;

SELECT '[two, three, five], 2, splitByNonAlpha';
SELECT 'lazy', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 2, 'splitByNonAlpha') SETTINGS text_index_posting_list_apply_mode = 'lazy', query_plan_optimize_count_from_text_index = 0;
SELECT 'materialize', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 2, 'splitByNonAlpha') SETTINGS text_index_posting_list_apply_mode = 'materialize', query_plan_optimize_count_from_text_index = 0;
SELECT 'count lazy', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 2, 'splitByNonAlpha') SETTINGS text_index_posting_list_apply_mode = 'lazy', query_plan_optimize_count_from_text_index = 1;
SELECT 'count materialize', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 2, 'splitByNonAlpha') SETTINGS text_index_posting_list_apply_mode = 'materialize', query_plan_optimize_count_from_text_index = 1;
SELECT 'scan', count() FROM tab WHERE hasAnyTokens(text, ['two', 'three', 'five'], 2, 'splitByNonAlpha') SETTINGS use_skip_indexes = 0, query_plan_direct_read_from_text_index = 0;

-- The same as the disjunction of the pairs.
SELECT count() FROM tab WHERE hasAllTokens(text, ['two', 'three']) OR hasAllTokens(text, ['two', 'five']) OR hasAllTokens(text, ['three', 'five']);

DROP TABLE tab;
