-- Direct read from text indexes when the parts of a table have different subsets of the indexes materialized.

SET use_skip_indexes = 1;
SET query_plan_direct_read_from_text_index = 1;
SET use_query_condition_cache = 0;

DROP TABLE IF EXISTS tab;

CREATE TABLE tab (id UInt64, k UInt8, a String, b String)
ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 64, add_minmax_index_for_numeric_columns = 0;

SYSTEM STOP MERGES tab;

-- Part 1: no index. Part 2: `idx_a`. Part 3: `idx_a` and `idx_b`.
INSERT INTO tab SELECT number, number % 7, concat('w', toString(number % 5)), concat('v', toString(number % 3)) FROM numbers(0, 1000);
ALTER TABLE tab ADD INDEX idx_a(a) TYPE text(tokenizer = splitByNonAlpha);
INSERT INTO tab SELECT number, number % 7, concat('w', toString(number % 5)), concat('v', toString(number % 3)) FROM numbers(1000, 1000);
ALTER TABLE tab ADD INDEX idx_b(b) TYPE text(tokenizer = splitByNonAlpha);
INSERT INTO tab SELECT number, number % 7, concat('w', toString(number % 5)), concat('v', toString(number % 3)) FROM numbers(2000, 1000);

SELECT count() > 0 FROM (EXPLAIN actions = 1 SELECT count() FROM tab WHERE hasToken(a, 'w1') AND hasToken(b, 'v2')) WHERE explain LIKE '%__text_index_idx_a%';
SELECT count() > 0 FROM (EXPLAIN actions = 1 SELECT count() FROM tab WHERE hasToken(a, 'w1') AND hasToken(b, 'v2')) WHERE explain LIKE '%__text_index_idx_b%';

SELECT 'and, direct read, on_data_read = 0', count() FROM tab WHERE hasToken(a, 'w1') AND hasToken(b, 'v2') SETTINGS query_plan_direct_read_from_text_index = 1, use_skip_indexes_on_data_read = 0;
SELECT 'and, direct read, on_data_read = 1', count() FROM tab WHERE hasToken(a, 'w1') AND hasToken(b, 'v2') SETTINGS query_plan_direct_read_from_text_index = 1, use_skip_indexes_on_data_read = 1;
SELECT 'and, no direct read', count() FROM tab WHERE hasToken(a, 'w1') AND hasToken(b, 'v2') SETTINGS query_plan_direct_read_from_text_index = 0;

SELECT 'or, direct read, on_data_read = 0', count() FROM tab WHERE hasToken(a, 'w1') OR hasToken(b, 'v2') SETTINGS query_plan_direct_read_from_text_index = 1, use_skip_indexes_on_data_read = 0;
SELECT 'or, direct read, on_data_read = 1', count() FROM tab WHERE hasToken(a, 'w1') OR hasToken(b, 'v2') SETTINGS query_plan_direct_read_from_text_index = 1, use_skip_indexes_on_data_read = 1;
SELECT 'or, no direct read', count() FROM tab WHERE hasToken(a, 'w1') OR hasToken(b, 'v2') SETTINGS query_plan_direct_read_from_text_index = 0;

SELECT 'prewhere, direct read, on_data_read = 0', count() FROM tab PREWHERE hasToken(a, 'w1') AND k = 1 AND hasToken(b, 'v2') SETTINGS query_plan_direct_read_from_text_index = 1, use_skip_indexes_on_data_read = 0;
SELECT 'prewhere, direct read, on_data_read = 1', count() FROM tab PREWHERE hasToken(a, 'w1') AND k = 1 AND hasToken(b, 'v2') SETTINGS query_plan_direct_read_from_text_index = 1, use_skip_indexes_on_data_read = 1;
SELECT 'prewhere, no direct read', count() FROM tab PREWHERE hasToken(a, 'w1') AND k = 1 AND hasToken(b, 'v2') SETTINGS query_plan_direct_read_from_text_index = 0;

SELECT 'select columns, direct read, on_data_read = 0', count(), sum(length(a)), sum(id) FROM tab WHERE hasToken(b, 'v0') AND k = 3 AND hasToken(a, 'w2') SETTINGS query_plan_direct_read_from_text_index = 1, use_skip_indexes_on_data_read = 0;
SELECT 'select columns, direct read, on_data_read = 1', count(), sum(length(a)), sum(id) FROM tab WHERE hasToken(b, 'v0') AND k = 3 AND hasToken(a, 'w2') SETTINGS query_plan_direct_read_from_text_index = 1, use_skip_indexes_on_data_read = 1;
SELECT 'select columns, no direct read', count(), sum(length(a)), sum(id) FROM tab WHERE hasToken(b, 'v0') AND k = 3 AND hasToken(a, 'w2') SETTINGS query_plan_direct_read_from_text_index = 0;

DROP TABLE tab;
