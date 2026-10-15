#!/usr/bin/env bash

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# The element size of `PFor` comes from the column type; `clickhouse-compressor` has no type and works
# on 1-byte elements, so the input is a stream of bytes in long runs. The mode is mandatory.

input="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.bin"
$CLICKHOUSE_LOCAL -q "SELECT toUInt8(intDiv(number, 1000)) FROM numbers(8000000) FORMAT RowBinary" > "$input"

function round_trip()
{
    local codec=$1
    rm -f "$input.compressed" "$input.decompressed"
    $CLICKHOUSE_COMPRESSOR --codec "$codec" --input "$input" --output "$input.compressed"
    $CLICKHOUSE_COMPRESSOR --decompress --input "$input.compressed" --output "$input.decompressed"
    local input_size compressed_size
    input_size=$(stat -c %s "$input")
    compressed_size=$(stat -c %s "$input.compressed")
    if cmp -s "$input" "$input.decompressed"; then
        echo "$codec: round trip ok, compressed to less than 1/8: $((compressed_size * 8 < input_size))"
    else
        echo "$codec: round trip FAILED"
    fi
}

round_trip "PFor('double_delta')"
round_trip "PFor('delta')"
round_trip "PFor('none')"

echo '-- invalid arguments'
$CLICKHOUSE_COMPRESSOR --codec 'PFor' --input "$input" --output "$input.compressed" 2>&1 | grep -c "ILLEGAL_SYNTAX_FOR_CODEC_TYPE"
$CLICKHOUSE_COMPRESSOR --codec 'PFor(8)' --input "$input" --output "$input.compressed" 2>&1 | grep -c "ILLEGAL_CODEC_PARAMETER"
$CLICKHOUSE_COMPRESSOR --codec "PFor('unknown')" --input "$input" --output "$input.compressed" 2>&1 | grep -c "ILLEGAL_CODEC_PARAMETER"
$CLICKHOUSE_COMPRESSOR --codec "PFor('delta', 8)" --input "$input" --output "$input.compressed" 2>&1 | grep -c "ILLEGAL_SYNTAX_FOR_CODEC_TYPE"

rm -f "$input" "$input.compressed" "$input.decompressed"
