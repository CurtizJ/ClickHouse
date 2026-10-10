-- Virtual columns of a direct read from a text index that are evaluated from the original predicate in a part
-- (a bypassed dictionary scan of a pattern) next to columns of the same index read from posting lists, consumed in
-- PREWHERE, in WHERE and in the result, and combined with a lightweight delete.

SET use_skip_indexes = 1;
SET query_plan_direct_read_from_text_index = 1;
SET use_query_condition_cache = 0;
SET enable_multiple_prewhere_read_steps = 1;
SET max_threads = 1;
SET enable_parallel_replicas = 0;
SET use_text_index_like_evaluation_by_dictionary_scan = 1;
SET text_index_like_min_pattern_length = 1;
-- Abandon the dictionary scan on the first token with a non-embedded posting list.
SET text_index_like_max_postings_to_read = 0;
SET use_text_index_pattern_bypass_cache = 0;

DROP TABLE IF EXISTS tab;

CREATE TABLE tab
(
    id UInt64,
    k UInt8,
    s String,
    INDEX idx(s) TYPE text(tokenizer = array)
)
ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 128, min_bytes_for_wide_part = 0, add_minmax_index_for_numeric_columns = 0;

-- 100 distinct tokens of 100 rows each, so their posting lists are not embedded into the dictionary.
INSERT INTO tab SELECT number, number % 7, concat('ab', toString(number % 100)) FROM numbers(10000);

SELECT 'pattern and equality on one index';
SELECT count(), sum(id) FROM tab WHERE s LIKE '%b1%' AND s = 'ab15' SETTINGS use_skip_indexes_on_data_read = 0, log_comment = '05359_mixed_0';
SELECT count(), sum(id) FROM tab WHERE s LIKE '%b1%' AND s = 'ab15' SETTINGS use_skip_indexes_on_data_read = 1, log_comment = '05359_mixed_1';
SELECT count(), sum(id) FROM tab WHERE s LIKE '%b1%' AND s = 'ab15' SETTINGS use_skip_indexes = 0;
SELECT count(), sum(id) FROM tab WHERE s LIKE '%b2%' OR s = 'ab15' SETTINGS use_skip_indexes_on_data_read = 0;
SELECT count(), sum(id) FROM tab WHERE s LIKE '%b2%' OR s = 'ab15' SETTINGS use_skip_indexes_on_data_read = 1;
SELECT count(), sum(id) FROM tab WHERE s LIKE '%b2%' OR s = 'ab15' SETTINGS use_skip_indexes = 0;

-- The pattern is evaluated from the predicate, the equality is still read from the index.
SYSTEM FLUSH LOGS query_log;
SELECT log_comment, ProfileEvents['TextIndexDirectReadFallbackColumns'], ProfileEvents['TextIndexReaderTotalMicroseconds'] > 0
FROM system.query_log
WHERE current_database = currentDatabase() AND event_date >= yesterday() AND type = 'QueryFinish' AND log_comment LIKE '05359\_mixed\_%'
ORDER BY log_comment;

SELECT 'condition in WHERE';
SELECT count(), sum(id) FROM tab WHERE s LIKE '%b1%' AND k = 3 SETTINGS optimize_move_to_prewhere = 0, use_skip_indexes_on_data_read = 0;
SELECT count(), sum(id) FROM tab WHERE s LIKE '%b1%' AND k = 3 SETTINGS optimize_move_to_prewhere = 0, use_skip_indexes_on_data_read = 1;
SELECT count(), sum(id) FROM tab WHERE s LIKE '%b1%' AND k = 3 SETTINGS optimize_move_to_prewhere = 0, use_skip_indexes = 0;

SELECT 'condition in the result';
SELECT countIf(s LIKE '%b1%'), sum(id * (s LIKE '%b1%')) FROM tab WHERE k = 3 AND s LIKE '%b%' SETTINGS use_skip_indexes_on_data_read = 0;
SELECT countIf(s LIKE '%b1%'), sum(id * (s LIKE '%b1%')) FROM tab WHERE k = 3 AND s LIKE '%b%' SETTINGS use_skip_indexes_on_data_read = 1;
SELECT countIf(s LIKE '%b1%'), sum(id * (s LIKE '%b1%')) FROM tab WHERE k = 3 AND s LIKE '%b%' SETTINGS use_skip_indexes = 0;

SELECT 'lightweight delete';
DELETE FROM tab WHERE id % 5 = 0;
SELECT count(), sum(id) FROM tab PREWHERE s LIKE '%b1%' AND k = 3 SETTINGS use_skip_indexes_on_data_read = 0;
SELECT count(), sum(id) FROM tab PREWHERE s LIKE '%b1%' AND k = 3 SETTINGS use_skip_indexes_on_data_read = 1;
SELECT count(), sum(id) FROM tab PREWHERE s LIKE '%b1%' AND k = 3 SETTINGS use_skip_indexes = 0;

DROP TABLE tab;
