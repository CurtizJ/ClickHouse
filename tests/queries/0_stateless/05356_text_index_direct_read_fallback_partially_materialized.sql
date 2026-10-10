-- In a part where the text index is not materialized, the condition on its virtual column is evaluated from the
-- original predicate after the other PREWHERE conditions, so the indexed column is read only for the surviving granules.

SET use_skip_indexes = 1;
SET query_plan_direct_read_from_text_index = 1;
SET use_query_condition_cache = 0;
SET optimize_move_to_prewhere = 1;
SET enable_multiple_prewhere_read_steps = 1;
SET max_threads = 1;
SET enable_parallel_replicas = 0;
SET merge_tree_read_split_ranges_into_intersecting_and_non_intersecting_injection_probability = 0;

DROP TABLE IF EXISTS tab;

CREATE TABLE tab (id UInt64, k UInt8, s String)
ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 100, index_granularity_bytes = '10Mi', min_bytes_for_wide_part = 0, add_minmax_index_for_numeric_columns = 0;

SYSTEM STOP MERGES tab;

-- Part without the index: 10000 rows, `k = 1` only in the first granule, the token is in every row.
INSERT INTO tab SELECT number, if(number < 100, 1, 2), concat('tok ', toString(number)) FROM numbers(10000);
ALTER TABLE tab ADD INDEX idx(s) TYPE text(tokenizer = splitByNonAlpha);
-- Part with the index: 100 rows.
INSERT INTO tab SELECT number + 10000, 1, concat('tok ', toString(number)) FROM numbers(100);

-- The text condition goes first in the PREWHERE of the query, but last in the part without the index.
SELECT count() FROM tab PREWHERE hasToken(s, 'tok') AND k = 1 SETTINGS use_skip_indexes_on_data_read = 0, log_comment = '05356_on_data_read_0';
SELECT count() FROM tab PREWHERE hasToken(s, 'tok') AND k = 1 SETTINGS use_skip_indexes_on_data_read = 1, log_comment = '05356_on_data_read_1';
SELECT count() FROM tab PREWHERE hasToken(s, 'tok') AND k = 1 SETTINGS query_plan_direct_read_from_text_index = 0;

SELECT sum(length(s)) FROM tab PREWHERE hasToken(s, 'tok') AND k = 1 SETTINGS use_skip_indexes_on_data_read = 0;
SELECT sum(length(s)) FROM tab WHERE hasToken(s, 'tok') AND k = 1 SETTINGS use_skip_indexes_on_data_read = 1;
SELECT sum(length(s)) FROM tab WHERE hasToken(s, 'tok') AND k = 1 SETTINGS query_plan_direct_read_from_text_index = 0;

SYSTEM FLUSH LOGS query_log;

-- `k` is read for all 10100 rows, `s` only for the granule where `k = 1` and for the small part.
-- Evaluating the text condition first would read `s` for all 10000 rows of the part without the index.
-- With `use_skip_indexes_on_data_read = 1` the reader of the index analysis counts each selected row once more.
SELECT
    log_comment,
    ProfileEvents['TextIndexDirectReadFallbackColumns'],
    ProfileEvents['RowsReadByPrewhereReaders'] < if(log_comment = '05356_on_data_read_0', 1.5, 2.5) * ProfileEvents['SelectedRows']
FROM system.query_log
WHERE current_database = currentDatabase() AND event_date >= yesterday() AND type = 'QueryFinish' AND log_comment LIKE '05356_on_data_read_%'
ORDER BY log_comment;

DROP TABLE tab;
