-- A materialized text index whose analysis decides that a virtual column is evaluated from the original predicate
-- (a bypassed dictionary scan of a pattern, a frequent phrase): the condition is evaluated after the other PREWHERE
-- conditions by a normal reader, so the indexed column is read only for the surviving granules.

SET use_skip_indexes = 1;
SET query_plan_direct_read_from_text_index = 1;
SET use_query_condition_cache = 0;
SET enable_multiple_prewhere_read_steps = 1;
SET max_threads = 1;
SET enable_parallel_replicas = 0;
SET merge_tree_read_split_ranges_into_intersecting_and_non_intersecting_injection_probability = 0;
SET use_text_index_like_evaluation_by_dictionary_scan = 1;
-- Abandon the dictionary scan on the first token with a non-embedded posting list.
SET text_index_like_max_postings_to_read = 0;
SET use_text_index_pattern_bypass_cache = 0;

DROP TABLE IF EXISTS tab;

CREATE TABLE tab
(
    id UInt64,
    k UInt8,
    s String,
    INDEX idx(s) TYPE text(tokenizer = splitByNonAlpha, support_phrase_search = 1)
)
ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 100, index_granularity_bytes = '10Mi', min_bytes_for_wide_part = 0, add_minmax_index_for_numeric_columns = 0, allow_experimental_text_index_phrase_search = 1;

-- `k = 1` only in the first granule.
INSERT INTO tab SELECT number, if(number < 100, 1, 2), concat('token', toString(number % 100), ' payload word') FROM numbers(10000);

SELECT 'like';
SELECT count() FROM tab PREWHERE s LIKE '%token4%' AND k = 1 SETTINGS use_skip_indexes_on_data_read = 0, log_comment = '05358_like_0';
SELECT count() FROM tab PREWHERE s LIKE '%token4%' AND k = 1 SETTINGS use_skip_indexes_on_data_read = 1, log_comment = '05358_like_1';
SELECT count() FROM tab PREWHERE s LIKE '%token4%' AND k = 1 SETTINGS use_skip_indexes = 0;

SELECT 'phrase';
SELECT count() FROM tab PREWHERE hasPhrase(s, 'payload word') AND k = 1 SETTINGS text_index_hint_max_selectivity = 0, use_skip_indexes_on_data_read = 0, log_comment = '05358_phrase_0';
SELECT count() FROM tab PREWHERE hasPhrase(s, 'payload word') AND k = 1 SETTINGS text_index_hint_max_selectivity = 0, use_skip_indexes_on_data_read = 1, log_comment = '05358_phrase_1';
SELECT count() FROM tab PREWHERE hasPhrase(s, 'payload word') AND k = 1 SETTINGS use_skip_indexes = 0;

SELECT 'select the indexed column';
SELECT sum(length(s)) FROM tab PREWHERE s LIKE '%token4%' AND k = 1 SETTINGS use_skip_indexes_on_data_read = 0, log_comment = '05358_select_0';
SELECT sum(length(s)) FROM tab PREWHERE s LIKE '%token4%' AND k = 1 SETTINGS use_skip_indexes_on_data_read = 1, log_comment = '05358_select_1';
SELECT sum(length(s)) FROM tab PREWHERE s LIKE '%token4%' AND k = 1 SETTINGS use_skip_indexes = 0;

SYSTEM FLUSH LOGS query_log;

-- `s` is read only for the granule where `k = 1`. Evaluating the text condition first would read it for all rows.
-- With `use_skip_indexes_on_data_read = 1` the index is analyzed at read time, and the reader of that analysis
-- counts each selected row once more.
SELECT
    log_comment,
    ProfileEvents['TextIndexDirectReadFallbackColumns'],
    ProfileEvents['TextIndexPhraseFallbacks'],
    ProfileEvents['RowsReadByPrewhereReaders'] < if(endsWith(log_comment, '_0'), 1.5, 2.5) * ProfileEvents['SelectedRows']
FROM system.query_log
WHERE current_database = currentDatabase() AND event_date >= yesterday() AND type = 'QueryFinish' AND log_comment IN ('05358_like_0', '05358_like_1', '05358_phrase_0', '05358_phrase_1')
ORDER BY log_comment;

-- `s` is read once, by the step that evaluates the pattern, so the main reader has nothing left to read.
SELECT log_comment, ProfileEvents['TextIndexDirectReadFallbackColumns'], ProfileEvents['RowsReadByMainReader']
FROM system.query_log
WHERE current_database = currentDatabase() AND event_date >= yesterday() AND type = 'QueryFinish' AND log_comment IN ('05358_select_0', '05358_select_1')
ORDER BY log_comment;

DROP TABLE tab;
