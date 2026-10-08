#!/usr/bin/env bash

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# A part left by the released #109595 bug holds skip-index files on disk that are missing from
# `checksums.txt`. A mutation must not hardlink such orphans into the new part, including the
# positions substream (`.pos`) of a text index that was redefined without `support_phrase_search`,
# which the current definition does not write.

data_path="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
saved_path="${data_path}_saved"
rm -rf "${data_path:?}" "${saved_path:?}"
mkdir -p "$saved_path"

active_part_path()
{
    $CLICKHOUSE_LOCAL --path "$data_path" -q "SELECT path FROM system.parts WHERE database = currentDatabase() AND table = 'tab' AND active"
}

$CLICKHOUSE_LOCAL --path "$data_path" -m -q "
CREATE TABLE tab
(
    id UInt32,
    body String,
    w UInt32,
    INDEX idx body TYPE text(tokenizer = 'splitByNonAlpha', support_phrase_search = 1)
)
ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 64, min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0, replace_long_file_name_to_hash = 0,
    allow_experimental_text_index_phrase_search = 1;
INSERT INTO tab SELECT number, concat('word', toString(number % 100), ' common'), number FROM numbers(1000);
"

part_path=$(active_part_path)
cp "${part_path}"skp_idx_idx.pos.* "$saved_path"/
ls "$saved_path"

# Redefine the index without phrase search, then put the positions files back on disk without checksums.
$CLICKHOUSE_LOCAL --path "$data_path" -m -q "
ALTER TABLE tab DROP INDEX idx SETTINGS mutations_sync = 2;
ALTER TABLE tab ADD INDEX idx body TYPE text(tokenizer = 'splitByNonAlpha');
"

part_path=$(active_part_path)
cp "$saved_path"/skp_idx_idx.pos.* "${part_path}"

# Updating a column that is not indexed hardlinks the index files into the new part.
$CLICKHOUSE_LOCAL --path "$data_path" -m -q "
ALTER TABLE tab UPDATE w = w + 1 WHERE 1 SETTINGS mutations_sync = 2;
"

part_path=$(active_part_path)
echo "-- orphans in the new part"
find "${part_path}" -maxdepth 1 -name 'skp_idx_idx.pos.*' | wc -l

$CLICKHOUSE_LOCAL --path "$data_path" -m -q "
CHECK TABLE tab SETTINGS check_query_single_value_result = 1;
SELECT count() FROM tab WHERE hasToken(body, 'word42');
"

rm -rf "${data_path:?}" "${saved_path:?}"
