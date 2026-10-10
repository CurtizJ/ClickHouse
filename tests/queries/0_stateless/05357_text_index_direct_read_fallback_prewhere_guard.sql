-- A condition on a text index virtual column that is evaluated from the original predicate in a part is moved later,
-- but never past a condition that may throw: the rows it rejects must not reach `intDiv`.

SET use_skip_indexes = 1;
SET query_plan_direct_read_from_text_index = 1;
SET use_query_condition_cache = 0;
SET enable_multiple_prewhere_read_steps = 1;

DROP TABLE IF EXISTS tab;

CREATE TABLE tab (id UInt64, k UInt8, d UInt8, s String)
ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 64, min_bytes_for_wide_part = 0, add_minmax_index_for_numeric_columns = 0;

SYSTEM STOP MERGES tab;

-- `d = 0` exactly in the rows without the token `ab`.
INSERT INTO tab SELECT number, number % 5, number % 3 != 0, if(number % 3 = 0, 'xy', 'ab') FROM numbers(1000);
ALTER TABLE tab ADD INDEX idx(s) TYPE text(tokenizer = splitByNonAlpha);
INSERT INTO tab SELECT number + 1000, number % 5, number % 3 != 0, if(number % 3 = 0, 'xy', 'ab') FROM numbers(1000);

SELECT count() FROM tab PREWHERE hasToken(s, 'ab') AND k != 7 AND intDiv(10, d) > 0 SETTINGS use_skip_indexes_on_data_read = 0;
SELECT count() FROM tab PREWHERE hasToken(s, 'ab') AND k != 7 AND intDiv(10, d) > 0 SETTINGS use_skip_indexes_on_data_read = 1;
SELECT count() FROM tab PREWHERE hasToken(s, 'ab') AND k != 7 AND intDiv(10, d) > 0 SETTINGS query_plan_direct_read_from_text_index = 0;

DROP TABLE tab;

-- The same with a materialized index whose dictionary scan of the pattern is abandoned, in both index analysis modes.
CREATE TABLE tab (id UInt64, k UInt8, d UInt8, s String, INDEX idx(s) TYPE text(tokenizer = array))
ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 64, min_bytes_for_wide_part = 0, add_minmax_index_for_numeric_columns = 0;

-- `d = 0` exactly in the rows that do not match the pattern. Each token has 100 rows, so its posting list is not embedded.
INSERT INTO tab SELECT number, number % 5, number % 3 != 0, if(number % 3 = 0, concat('xy', toString(number % 10)), concat('ab', toString(number % 10))) FROM numbers(1000);

SELECT count() FROM tab PREWHERE s LIKE '%ab%' AND k != 7 AND intDiv(10, d) > 0
SETTINGS use_skip_indexes_on_data_read = 0, use_text_index_like_evaluation_by_dictionary_scan = 1, text_index_like_min_pattern_length = 1, text_index_like_max_postings_to_read = 0, use_text_index_pattern_bypass_cache = 0;
SELECT count() FROM tab PREWHERE s LIKE '%ab%' AND k != 7 AND intDiv(10, d) > 0
SETTINGS use_skip_indexes_on_data_read = 1, use_text_index_like_evaluation_by_dictionary_scan = 1, text_index_like_min_pattern_length = 1, text_index_like_max_postings_to_read = 0, use_text_index_pattern_bypass_cache = 0;
SELECT count() FROM tab PREWHERE s LIKE '%ab%' AND k != 7 AND intDiv(10, d) > 0 SETTINGS query_plan_direct_read_from_text_index = 0;

DROP TABLE tab;
