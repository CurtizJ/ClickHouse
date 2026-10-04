#!/usr/bin/env bash

# Skip index prefetching (`use_skip_indexes_prefetch`): the results are equal with and without it,
# and the prefetches are issued when the read method prefetches asynchronously.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

$CLICKHOUSE_CLIENT --query "
    DROP TABLE IF EXISTS t_skip_index_prefetch;
    DROP TABLE IF EXISTS t_skip_index_prefetch_final;

    CREATE TABLE t_skip_index_prefetch
    (
        id UInt64,
        v UInt64,
        s String,
        k UInt64,
        INDEX idx_v v TYPE minmax GRANULARITY 1,
        INDEX idx_s s TYPE bloom_filter GRANULARITY 1,
        INDEX idx_k k TYPE set(100) GRANULARITY 2
    )
    ENGINE = MergeTree ORDER BY id
    SETTINGS index_granularity = 256, index_granularity_bytes = '100Mi', max_bytes_to_merge_at_max_space_in_pool = 1;

    CREATE TABLE t_skip_index_prefetch_final
    (
        id UInt64,
        v UInt64,
        s String,
        INDEX idx_s s TYPE bloom_filter GRANULARITY 1
    )
    ENGINE = ReplacingMergeTree ORDER BY id
    SETTINGS index_granularity = 256, index_granularity_bytes = '100Mi', max_bytes_to_merge_at_max_space_in_pool = 1;
"

for i in $(seq 0 5); do
    $CLICKHOUSE_CLIENT --query "
        INSERT INTO t_skip_index_prefetch
        SELECT number + $i * 50000, number, concat('filler_', toString(number % 3000)), intDiv(number, 2048) FROM numbers(50000)"
    $CLICKHOUSE_CLIENT --query "
        INSERT INTO t_skip_index_prefetch_final
        SELECT number * 3 + $i, $i, concat('row_', toString(number % 5000)) FROM numbers(20000)"
done

# The pins make the parts prefetchable on a local disk and on S3 alike, and keep the analysis deterministic.
common_settings="enable_analyzer = 1, use_skip_indexes = 1, use_query_condition_cache = 0, max_rows_to_read = 0,
    enable_parallel_replicas = 0, use_reader_executor = 0, max_threads = 4,
    local_filesystem_read_method = 'pread_threadpool', local_filesystem_read_prefetch = 1,
    remote_filesystem_read_method = 'threadpool', remote_filesystem_read_prefetch = 1,
    merge_tree_read_split_ranges_into_intersecting_and_non_intersecting_injection_probability = 0"

function check_equal_results()
{
    local name="$1"
    local query="$2"
    local settings="$3"

    local res_prefetch
    local res_sync
    res_prefetch=$($CLICKHOUSE_CLIENT --query "$query SETTINGS $common_settings, $settings, use_skip_indexes_prefetch = 1, log_comment = 'prefetch_test_${name}_on'")
    res_sync=$($CLICKHOUSE_CLIENT --query "$query SETTINGS $common_settings, $settings, use_skip_indexes_prefetch = 0, log_comment = 'prefetch_test_${name}_off'")

    if [ "$res_prefetch" == "$res_sync" ]; then
        echo "$name $res_prefetch"
    else
        echo "$name MISMATCH: prefetch='$res_prefetch' sync='$res_sync'"
    fi
}

query="SELECT count(), sum(v) FROM t_skip_index_prefetch WHERE v >= 10000 AND s = 'filler_42' AND k IN (5, 6, 7)"

check_equal_results "parallel" "$query" "use_skip_indexes_on_data_read = 0, max_threads_for_indexes = 4"
check_equal_results "serial" "$query" "use_skip_indexes_on_data_read = 0, max_threads_for_indexes = 1"
check_equal_results "budget_1" "$query" "use_skip_indexes_on_data_read = 0, max_threads_for_indexes = 1, filesystem_prefetches_limit = 1"
check_equal_results "data_read" "$query" "use_skip_indexes_on_data_read = 1"

check_equal_results "disjunction" \
    "SELECT count(), sum(v) FROM t_skip_index_prefetch WHERE v >= 40000 AND (s = 'filler_77' OR k = 21)" \
    "use_skip_indexes_on_data_read = 0, use_skip_indexes_for_disjunctions = 1, max_threads_for_indexes = 4"

check_equal_results "final" \
    "SELECT count(), sum(v) FROM t_skip_index_prefetch_final FINAL WHERE s = 'row_123'" \
    "use_skip_indexes_on_data_read = 0, use_skip_indexes_if_final = 1, use_skip_indexes_if_final_exact_mode = 1"

$CLICKHOUSE_CLIENT --query "SYSTEM FLUSH LOGS query_log"

$CLICKHOUSE_CLIENT --query "
    SELECT
        replaceOne(log_comment, 'prefetch_test_', '') AS name,
        ProfileEvents['SkipIndexPrefetches'] > 0 AS prefetches,
        ProfileEvents['SkipIndexPrefetchesRunInline'] <= ProfileEvents['SkipIndexPrefetches'] AS run_inline_le_prefetches,
        ProfileEvents['SkipIndexPrefetchBudgetExhausted'] > 0 AS budget_exhausted
    FROM system.query_log
    WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment LIKE 'prefetch_test_%'
        AND log_comment NOT LIKE '%data_read%'
    ORDER BY name
"

$CLICKHOUSE_CLIENT --query "
    DROP TABLE t_skip_index_prefetch;
    DROP TABLE t_skip_index_prefetch_final;
"
