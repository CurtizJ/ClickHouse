-- The virtual columns of a text index are filled by a separate read step. With the dynamic top-K filter
-- of `ORDER BY <column> LIMIT n` in the first PREWHERE read step, the index step runs after it and decodes
-- the posting lists only for the granules with rows above the threshold.

DROP TABLE IF EXISTS t_top_k_text_index_order;

-- Small posting list blocks, so that the number of decoded blocks follows the number of granules read by the index step.
CREATE TABLE t_top_k_text_index_order (key UInt64, value UInt64, s String, INDEX idx_s s TYPE text(tokenizer = splitByNonAlpha, posting_list_codec = 'bitpacking', posting_list_block_size = 1024))
ENGINE = MergeTree ORDER BY key
SETTINGS index_granularity = 1024, index_granularity_bytes = '10M';

-- `value` decreases with `key`, so the first granules hold the largest values and the threshold
-- of `ORDER BY value DESC` is final after the first block.
INSERT INTO t_top_k_text_index_order SELECT number, 1000000 - number, if(number % 2 = 0, 'even row', 'odd row') FROM numbers(200000);

SET use_top_k_dynamic_filtering = 1, use_skip_indexes_for_top_k = 0, use_query_condition_cache = 0, enable_parallel_replicas = 0, max_threads = 1;
SET use_skip_indexes_on_data_read = 1, query_plan_direct_read_from_text_index = 1, use_text_index_postings_cache = 0;
SET merge_tree_read_split_ranges_into_intersecting_and_non_intersecting_injection_probability = 0;

SELECT '-- results';
SELECT key, value FROM t_top_k_text_index_order WHERE hasToken(s, 'even') ORDER BY value DESC LIMIT 5;
SELECT key, value FROM t_top_k_text_index_order WHERE hasToken(s, 'even') ORDER BY value DESC LIMIT 5 SETTINGS use_top_k_dynamic_filtering = 0;

SELECT key FROM t_top_k_text_index_order WHERE hasToken(s, 'even') ORDER BY value DESC LIMIT 10 FORMAT Null
    SETTINGS log_comment = '05232_lazy_on', text_index_posting_list_apply_mode = 'lazy';
SELECT key FROM t_top_k_text_index_order WHERE hasToken(s, 'even') ORDER BY value DESC LIMIT 10 FORMAT Null
    SETTINGS log_comment = '05232_lazy_off', text_index_posting_list_apply_mode = 'lazy', use_top_k_dynamic_filtering = 0;
SELECT key FROM t_top_k_text_index_order WHERE hasToken(s, 'even') ORDER BY value DESC LIMIT 10 FORMAT Null
    SETTINGS log_comment = '05232_materialize_on', text_index_posting_list_apply_mode = 'materialize';
SELECT key FROM t_top_k_text_index_order WHERE hasToken(s, 'even') ORDER BY value DESC LIMIT 10 FORMAT Null
    SETTINGS log_comment = '05232_materialize_off', text_index_posting_list_apply_mode = 'materialize', use_top_k_dynamic_filtering = 0;

SYSTEM FLUSH LOGS query_log;

-- Without the dynamic filter the index step decodes the posting lists of every granule; with it, only of the
-- granules left by the top-K filter, i.e. of the first block.
SELECT '-- lazy mode, posting list blocks decoded: with the top-K filter > 0 and < without it';
WITH
    (SELECT ProfileEvents['TextIndexLazyPackedBlocksDecoded'] FROM system.query_log
        WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment = '05232_lazy_on' ORDER BY event_time_microseconds DESC LIMIT 1) AS blocks_on,
    (SELECT ProfileEvents['TextIndexLazyPackedBlocksDecoded'] FROM system.query_log
        WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment = '05232_lazy_off' ORDER BY event_time_microseconds DESC LIMIT 1) AS blocks_off
SELECT blocks_on > 0, blocks_on < blocks_off;

SELECT '-- materialize mode, posting list blocks read: with the top-K filter > 0 and < without it';
WITH
    (SELECT ProfileEvents['TextIndexReadPostings'] FROM system.query_log
        WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment = '05232_materialize_on' ORDER BY event_time_microseconds DESC LIMIT 1) AS blocks_on,
    (SELECT ProfileEvents['TextIndexReadPostings'] FROM system.query_log
        WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment = '05232_materialize_off' ORDER BY event_time_microseconds DESC LIMIT 1) AS blocks_off
SELECT blocks_on > 0, blocks_on < blocks_off;

DROP TABLE t_top_k_text_index_order;
