#!/usr/bin/env bash
# Tags: no-parallel

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

set -e

$CLICKHOUSE_CLIENT -n -q "
    DROP TABLE IF EXISTS t_async_insert_native;
    CREATE TABLE t_async_insert_native (id UInt64, s String) ENGINE = MergeTree ORDER BY id;
"

async_insert_options="--async_insert 1 --wait_for_async_insert 0 --async_insert_busy_timeout_ms 1000000"

echo '{"id": 1, "s": "aaa"} {"id": 2, "s": "bbb"}' | $CLICKHOUSE_CLIENT $async_insert_options -q 'INSERT INTO t_async_insert_native FORMAT JSONEachRow'
$CLICKHOUSE_CLIENT $async_insert_options  -q 'INSERT INTO t_async_insert_native FORMAT JSONEachRow {"id": 3, "s": "ccc"}'

$CLICKHOUSE_CLIENT -n -q "
    SELECT sum(length(entries.bytes)) FROM system.asynchronous_inserts
    WHERE database = '$CLICKHOUSE_DATABASE' AND table = 't_async_insert_native';

    SYSTEM FLUSH ASYNC INSERT QUEUE;

    SELECT * FROM t_async_insert_native ORDER BY id;

    SYSTEM FLUSH LOGS;

    SELECT status, rows, data_kind, format
    FROM system.asynchronous_insert_log
    WHERE database = '$CLICKHOUSE_DATABASE' AND table = 't_async_insert_native'
    ORDER BY event_time_microseconds;

    DROP TABLE t_async_insert_native;
"
