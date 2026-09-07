-- Tags: no-parallel-replicas

-- Virtual columns of direct reading from the text index are produced as sparse columns when the
-- posting lists bound the matches to a small share of the part. Both apply modes must fill the sparse
-- columns correctly, and the ProfileEvent must fire only for the queries with a low estimated selectivity.

SET enable_analyzer = 1;
SET enable_full_text_index = 1;
SET use_skip_indexes = 1;
SET query_plan_direct_read_from_text_index = 1;
SET query_plan_optimize_count_from_text_index = 0;
SET use_query_condition_cache = 0;
SET merge_tree_read_split_ranges_into_intersecting_and_non_intersecting_injection_probability = 0.0;
SET log_comment = '05136_text_index_sparse_virtual_columns';

DROP TABLE IF EXISTS tab_sparse;

CREATE TABLE tab_sparse
(
    id UInt64,
    s String,
    INDEX idx_s (s) TYPE text(tokenizer = splitByNonAlpha, support_phrase_search = 1)
)
ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 1024, allow_experimental_text_index_phrase_search = 1;

-- `common` is in every row, `rare` in 1% of rows, `scarce` in a third of the `rare` rows.
INSERT INTO tab_sparse SELECT number, concat('common ', if(number % 100 = 7, 'rare ', ''), if(number % 300 = 7, 'scarce ', ''), 'tail') FROM numbers(20000);

SELECT '-- materialize';
SET text_index_posting_list_apply_mode = 'materialize';

SELECT count(), sum(id) FROM tab_sparse WHERE hasToken(s, 'rare');
SELECT count(), sum(id) FROM tab_sparse WHERE hasToken(s, 'common');
SELECT count(), sum(id) FROM tab_sparse WHERE hasAllTokens(s, ['common', 'rare']);
SELECT count(), sum(id) FROM tab_sparse WHERE hasAnyTokens(s, ['common', 'rare']);
SELECT count(), sum(id) FROM tab_sparse WHERE hasAnyTokens(s, ['rare', 'scarce']);
SELECT count(), sum(id) FROM tab_sparse WHERE hasPhrase(s, 'rare scarce');
SELECT count(), sum(id) FROM tab_sparse WHERE hasToken(s, 'rare') OR hasToken(s, 'scarce');
SELECT count(), sum(id) FROM tab_sparse WHERE hasToken(s, 'scarce') AND id % 2 = 1;
SELECT count(), sum(id) FROM tab_sparse WHERE s LIKE '%scarce%';
SELECT count(), sum(id) FROM tab_sparse WHERE hasToken(s, 'rare') SETTINGS text_index_ratio_of_defaults_for_sparse_columns = 1.0;

SELECT '-- lazy';
SET text_index_posting_list_apply_mode = 'lazy';

SELECT count(), sum(id) FROM tab_sparse WHERE hasToken(s, 'rare');
SELECT count(), sum(id) FROM tab_sparse WHERE hasToken(s, 'common');
SELECT count(), sum(id) FROM tab_sparse WHERE hasAllTokens(s, ['common', 'rare']);
SELECT count(), sum(id) FROM tab_sparse WHERE hasAnyTokens(s, ['common', 'rare']);
SELECT count(), sum(id) FROM tab_sparse WHERE hasAnyTokens(s, ['rare', 'scarce']);
SELECT count(), sum(id) FROM tab_sparse WHERE hasPhrase(s, 'rare scarce');
SELECT count(), sum(id) FROM tab_sparse WHERE hasToken(s, 'rare') OR hasToken(s, 'scarce');
SELECT count(), sum(id) FROM tab_sparse WHERE hasToken(s, 'scarce') AND id % 2 = 1;
SELECT count(), sum(id) FROM tab_sparse WHERE s LIKE '%scarce%';
SELECT count(), sum(id) FROM tab_sparse WHERE hasToken(s, 'rare') SETTINGS text_index_ratio_of_defaults_for_sparse_columns = 1.0;

SELECT '-- reference without the index';
SELECT count(), sum(id) FROM tab_sparse WHERE hasToken(s, 'rare') SETTINGS use_skip_indexes = 0;
SELECT count(), sum(id) FROM tab_sparse WHERE hasAnyTokens(s, ['rare', 'scarce']) SETTINGS use_skip_indexes = 0;
SELECT count(), sum(id) FROM tab_sparse WHERE hasPhrase(s, 'rare scarce') SETTINGS use_skip_indexes = 0;
SELECT count(), sum(id) FROM tab_sparse WHERE hasToken(s, 'scarce') AND id % 2 = 1 SETTINGS use_skip_indexes = 0;

SELECT '-- sparse virtual columns per query';
SYSTEM FLUSH LOGS query_log;

SELECT
    Settings['text_index_posting_list_apply_mode'],
    trim(TRAILING ';' FROM replaceOne(query, 'SELECT count(), sum(id) FROM tab_sparse WHERE ', '')),
    ProfileEvents['TextIndexSparseVirtualColumns'] > 0
FROM system.query_log
WHERE current_database = currentDatabase()
    AND log_comment = '05136_text_index_sparse_virtual_columns'
    AND type = 'QueryFinish'
    AND query LIKE 'SELECT count(), sum(id) FROM tab_sparse%'
    AND query NOT LIKE '%use_skip_indexes = 0%'
ORDER BY event_time_microseconds;

DROP TABLE tab_sparse;
