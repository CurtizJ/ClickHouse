#!/usr/bin/env bash

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# A corrupted `PFor` block must fail with `CANNOT_DECOMPRESS` instead of reading out of bounds.
# The checksum of the compressed block normally catches the corruption first, so the reads below
# disable it with `checksum_on_read` to reach the codec's own validation.

data_path="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
rm -rf "$data_path"

$CLICKHOUSE_LOCAL --path "$data_path" --enable_pfor_codec 1 -m -q "
CREATE TABLE tab (key UInt64, ts UInt64 CODEC(PFor('double_delta')))
ENGINE = MergeTree ORDER BY key
SETTINGS min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0, ratio_of_defaults_for_sparse_serialization = 1;
INSERT INTO tab SELECT number, 1700000000000 + number * 15000 + intHash32(number) % 200 FROM numbers(1000);
SELECT sum(ts) FROM tab;
"

column_file="$data_path/data/default/tab/all_1_1_0/ts.bin"
cp "$column_file" "$column_file.orig"

# The compressed block starts with a 16-byte checksum and a 9-byte envelope, followed by the codec payload:
# element size at offset 25, mode at 26, then the first PFor block header (base width at 27, number of exceptions at 28).
function read_corrupted()
{
    local offset=$1
    local byte=$2
    cp "$column_file.orig" "$column_file"
    printf "$byte" | dd of="$column_file" bs=1 seek="$offset" conv=notrunc status=none
    $CLICKHOUSE_LOCAL --path "$data_path" --checksum_on_read 0 -q "SELECT sum(ts) FROM tab" 2>&1 | grep -oE 'CANNOT_DECOMPRESS|CHECKSUM_DOESNT_MATCH' | head -1
}

echo '-- wrong element size'
read_corrupted 25 '\x03'
echo '-- wrong mode'
read_corrupted 26 '\x09'
echo '-- block width above 64 bits'
read_corrupted 27 '\x7f'
echo '-- block width of 64 bits does not fit in the stream'
read_corrupted 27 '\x40'
echo '-- more exceptions than values in a block'
read_corrupted 28 '\xff'

echo '-- the checksum catches the corruption when enabled'
cp "$column_file.orig" "$column_file"
printf '\x09' | dd of="$column_file" bs=1 seek=26 conv=notrunc status=none
$CLICKHOUSE_LOCAL --path "$data_path" -q "SELECT sum(ts) FROM tab" 2>&1 | grep -oE 'CANNOT_DECOMPRESS|CHECKSUM_DOESNT_MATCH' | head -1

echo '-- the intact part reads back'
cp "$column_file.orig" "$column_file"
$CLICKHOUSE_LOCAL --path "$data_path" -q "SELECT sum(ts) FROM tab"

rm -rf "$data_path"
