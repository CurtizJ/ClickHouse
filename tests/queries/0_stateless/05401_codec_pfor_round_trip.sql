-- Round trip of the `PFor` codec in its three modes on every supported element width,
-- compared against the same data stored without compression.

SET enable_pfor_codec = 1;

DROP TABLE IF EXISTS pfor_ref;
DROP TABLE IF EXISTS pfor_none;
DROP TABLE IF EXISTS pfor_delta;
DROP TABLE IF EXISTS pfor_dd;
DROP TABLE IF EXISTS pfor_ts;
DROP TABLE IF EXISTS pfor_desc;

-- Small compressed blocks, so that every column spans several blocks and the last PFor block of each is partial.
CREATE TABLE pfor_ref
(
    key UInt64,
    u8 UInt8 CODEC(NONE),
    u16 UInt16 CODEC(NONE),
    u32 UInt32 CODEC(NONE),
    u64 UInt64 CODEC(NONE),
    i8 Int8 CODEC(NONE),
    i16 Int16 CODEC(NONE),
    i32 Int32 CODEC(NONE),
    i64 Int64 CODEC(NONE),
    d Date CODEC(NONE),
    dt DateTime('UTC') CODEC(NONE),
    dt64 DateTime64(3, 'UTC') CODEC(NONE),
    e Enum8('a' = -3, 'b' = 0, 'c' = 100) CODEC(NONE),
    dec Decimal64(4) CODEC(NONE),
    f Float64 CODEC(NONE),
    n Nullable(Int64) CODEC(NONE)
)
ENGINE = MergeTree ORDER BY key
SETTINGS min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0, ratio_of_defaults_for_sparse_serialization = 1,
    index_granularity = 1000, min_compress_block_size = 1000, max_compress_block_size = 8000;

CREATE TABLE pfor_none
(
    key UInt64,
    u8 UInt8 CODEC(PFor('none')),
    u16 UInt16 CODEC(PFor('none')),
    u32 UInt32 CODEC(PFor('none')),
    u64 UInt64 CODEC(PFor('none')),
    i8 Int8 CODEC(PFor('none')),
    i16 Int16 CODEC(PFor('none')),
    i32 Int32 CODEC(PFor('none')),
    i64 Int64 CODEC(PFor('none')),
    d Date CODEC(PFor('none')),
    dt DateTime('UTC') CODEC(PFor('none')),
    dt64 DateTime64(3, 'UTC') CODEC(PFor('none')),
    e Enum8('a' = -3, 'b' = 0, 'c' = 100) CODEC(PFor('none')),
    dec Decimal64(4) CODEC(PFor('none')),
    f Float64 CODEC(PFor('none')),
    n Nullable(Int64) CODEC(PFor('none'))
)
ENGINE = MergeTree ORDER BY key
SETTINGS min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0, ratio_of_defaults_for_sparse_serialization = 1,
    index_granularity = 1000, min_compress_block_size = 1000, max_compress_block_size = 8000;

CREATE TABLE pfor_delta
(
    key UInt64,
    u8 UInt8 CODEC(PFor('delta')),
    u16 UInt16 CODEC(PFor('delta')),
    u32 UInt32 CODEC(PFor('delta')),
    u64 UInt64 CODEC(PFor('delta')),
    i8 Int8 CODEC(PFor('delta')),
    i16 Int16 CODEC(PFor('delta')),
    i32 Int32 CODEC(PFor('delta')),
    i64 Int64 CODEC(PFor('delta')),
    d Date CODEC(PFor('delta')),
    dt DateTime('UTC') CODEC(PFor('delta')),
    dt64 DateTime64(3, 'UTC') CODEC(PFor('delta')),
    e Enum8('a' = -3, 'b' = 0, 'c' = 100) CODEC(PFor('delta')),
    dec Decimal64(4) CODEC(PFor('delta')),
    f Float64 CODEC(PFor('delta')),
    n Nullable(Int64) CODEC(PFor('delta'))
)
ENGINE = MergeTree ORDER BY key
SETTINGS min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0, ratio_of_defaults_for_sparse_serialization = 1,
    index_granularity = 1000, min_compress_block_size = 1000, max_compress_block_size = 8000;

CREATE TABLE pfor_dd
(
    key UInt64,
    u8 UInt8 CODEC(PFor('double_delta')),
    u16 UInt16 CODEC(PFor('double_delta')),
    u32 UInt32 CODEC(PFor('double_delta')),
    u64 UInt64 CODEC(PFor('double_delta')),
    i8 Int8 CODEC(PFor('double_delta')),
    i16 Int16 CODEC(PFor('double_delta')),
    i32 Int32 CODEC(PFor('double_delta')),
    i64 Int64 CODEC(PFor('double_delta')),
    d Date CODEC(PFor('double_delta')),
    dt DateTime('UTC') CODEC(PFor('double_delta')),
    dt64 DateTime64(3, 'UTC') CODEC(PFor('double_delta')),
    e Enum8('a' = -3, 'b' = 0, 'c' = 100) CODEC(PFor('double_delta')),
    dec Decimal64(4) CODEC(PFor('double_delta')),
    f Float64 CODEC(PFor('double_delta')),
    n Nullable(Int64) CODEC(PFor('double_delta'))
)
ENGINE = MergeTree ORDER BY key
SETTINGS min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0, ratio_of_defaults_for_sparse_serialization = 1,
    index_granularity = 1000, min_compress_block_size = 1000, max_compress_block_size = 8000;

-- One part per value pattern: constant stride, jittered stride, random, constant, decreasing (negative deltas),
-- alternating sign, plus a single-row part and the extreme values of every width.
CREATE TEMPORARY TABLE pfor_patterns (key UInt64, v UInt64);
INSERT INTO pfor_patterns SELECT number, number * 1000 FROM numbers(10000);
INSERT INTO pfor_patterns SELECT 10000 + number, number * 15000 + intHash32(number) % 1000 FROM numbers(10000);
INSERT INTO pfor_patterns SELECT 20000 + number, intHash64(number) FROM numbers(10000);
INSERT INTO pfor_patterns SELECT 30000 + number, 42 FROM numbers(10000);
INSERT INTO pfor_patterns SELECT 40000 + number, toUInt64(-toInt64(number) * 7) FROM numbers(10000);
INSERT INTO pfor_patterns SELECT 50000 + number, toUInt64(if(number % 2 = 0, toInt64(number), -toInt64(number))) FROM numbers(10000);
INSERT INTO pfor_patterns SELECT 60000, 123456789;
INSERT INTO pfor_patterns
    WITH [0, 1, 127, 128, 255, 256, 32767, 32768, 65535, 65536, 2147483647, 2147483648, 4294967295, 4294967296, 9223372036854775807, 9223372036854775808, 18446744073709551615]::Array(UInt64) AS extremes
    SELECT 70000 + number, extremes[number + 1] FROM numbers(17);

INSERT INTO pfor_ref SELECT
    key,
    toUInt8(v), toUInt16(v), toUInt32(v), v,
    toInt8(v), toInt16(v), toInt32(v), toInt64(v),
    toDate(toUInt16(v)), toDateTime(toUInt32(v), 'UTC'), fromUnixTimestamp64Milli(toInt64(v % 4000000000000), 'UTC'),
    ['a', 'b', 'c'][v % 3 + 1], toDecimal64(toInt64(v % 1000000000), 4), toFloat64(toInt64(v)) / 3,
    if(v % 7 = 0, NULL, toInt64(v))
FROM pfor_patterns ORDER BY key;

-- Two parts per table, so that the merge below recompresses the data.
INSERT INTO pfor_none SELECT * FROM pfor_ref WHERE key % 2 = 0;
INSERT INTO pfor_none SELECT * FROM pfor_ref WHERE key % 2 = 1;
INSERT INTO pfor_delta SELECT * FROM pfor_ref WHERE key % 2 = 0;
INSERT INTO pfor_delta SELECT * FROM pfor_ref WHERE key % 2 = 1;
INSERT INTO pfor_dd SELECT * FROM pfor_ref WHERE key % 2 = 0;
INSERT INTO pfor_dd SELECT * FROM pfor_ref WHERE key % 2 = 1;

SELECT 'row counts', (SELECT count() FROM pfor_ref), (SELECT count() FROM pfor_none), (SELECT count() FROM pfor_delta), (SELECT count() FROM pfor_dd);

SELECT 'none differs from reference', count() FROM (SELECT * FROM pfor_ref EXCEPT SELECT * FROM pfor_none);
SELECT 'delta differs from reference', count() FROM (SELECT * FROM pfor_ref EXCEPT SELECT * FROM pfor_delta);
SELECT 'double_delta differs from reference', count() FROM (SELECT * FROM pfor_ref EXCEPT SELECT * FROM pfor_dd);

OPTIMIZE TABLE pfor_none FINAL;
OPTIMIZE TABLE pfor_delta FINAL;
OPTIMIZE TABLE pfor_dd FINAL;

SELECT 'parts after merge', (SELECT count() FROM system.parts WHERE database = currentDatabase() AND table = 'pfor_dd' AND active);
SELECT 'none differs from reference after merge', count() FROM (SELECT * FROM pfor_none EXCEPT SELECT * FROM pfor_ref);
SELECT 'delta differs from reference after merge', count() FROM (SELECT * FROM pfor_delta EXCEPT SELECT * FROM pfor_ref);
SELECT 'double_delta differs from reference after merge', count() FROM (SELECT * FROM pfor_dd EXCEPT SELECT * FROM pfor_ref);

-- The codec description keeps the mode.
CREATE TABLE pfor_desc
(
    a UInt32 CODEC(PFor('delta')),
    b UInt32 CODEC(PFor('none')),
    c UInt32 CODEC(PFor('double_delta')),
    d UInt32 CODEC(PFor('delta'), ZSTD(1))
)
ENGINE = MergeTree ORDER BY tuple();

SELECT name, compression_codec FROM system.columns WHERE database = currentDatabase() AND table = 'pfor_desc' ORDER BY name;

-- Timestamps with an almost constant stride: `PFor('double_delta')` must compress well and no worse than `DoubleDelta`.
CREATE TABLE pfor_ts
(
    key UInt64,
    ts_none DateTime64(3, 'UTC') CODEC(NONE),
    ts_dd DateTime64(3, 'UTC') CODEC(DoubleDelta),
    ts_pfor DateTime64(3, 'UTC') CODEC(PFor('double_delta'))
)
ENGINE = MergeTree ORDER BY key
SETTINGS min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0, ratio_of_defaults_for_sparse_serialization = 1;

INSERT INTO pfor_ts SELECT number, ts, ts, ts
FROM (SELECT number, fromUnixTimestamp64Milli(toInt64(1767225600000 + number * 15000 + intHash32(number) % 200), 'UTC') AS ts FROM numbers(100000));

SELECT column, column_data_uncompressed_bytes / column_data_compressed_bytes >= 6 AS compresses_well
FROM system.parts_columns
WHERE database = currentDatabase() AND table = 'pfor_ts' AND active AND column = 'ts_pfor';

SELECT 'PFor is not larger than DoubleDelta',
    (SELECT column_data_compressed_bytes FROM system.parts_columns WHERE database = currentDatabase() AND table = 'pfor_ts' AND active AND column = 'ts_pfor')
    <= (SELECT column_data_compressed_bytes FROM system.parts_columns WHERE database = currentDatabase() AND table = 'pfor_ts' AND active AND column = 'ts_dd');

-- Changing the codec of an existing column and back.
ALTER TABLE pfor_ts MODIFY COLUMN ts_none DateTime64(3, 'UTC') CODEC(PFor('double_delta'));
OPTIMIZE TABLE pfor_ts FINAL;
SELECT name, compression_codec FROM system.columns WHERE database = currentDatabase() AND table = 'pfor_ts' AND name = 'ts_none';
SELECT 'recompressed with PFor',
    (SELECT column_data_compressed_bytes FROM system.parts_columns WHERE database = currentDatabase() AND table = 'pfor_ts' AND active AND column = 'ts_none')
    = (SELECT column_data_compressed_bytes FROM system.parts_columns WHERE database = currentDatabase() AND table = 'pfor_ts' AND active AND column = 'ts_pfor');
SELECT 'differs after recompression', count() FROM pfor_ts WHERE ts_none != ts_pfor OR ts_none != ts_dd;

ALTER TABLE pfor_ts MODIFY COLUMN ts_none DateTime64(3, 'UTC') CODEC(NONE);
OPTIMIZE TABLE pfor_ts FINAL;
SELECT name, compression_codec FROM system.columns WHERE database = currentDatabase() AND table = 'pfor_ts' AND name = 'ts_none';
SELECT 'differs after decompression', count() FROM pfor_ts WHERE ts_none != ts_pfor OR ts_none != ts_dd;

-- Invalid arguments and types. The mode is mandatory.
CREATE TABLE pfor_bad (c UInt64 CODEC(PFor)) ENGINE = MergeTree ORDER BY tuple(); -- { serverError ILLEGAL_SYNTAX_FOR_CODEC_TYPE }
CREATE TABLE pfor_bad (c UInt64 CODEC(PFor())) ENGINE = MergeTree ORDER BY tuple(); -- { serverError ILLEGAL_SYNTAX_FOR_CODEC_TYPE }
CREATE TABLE pfor_bad (c String CODEC(PFor('delta'))) ENGINE = MergeTree ORDER BY tuple(); -- { serverError BAD_ARGUMENTS }
CREATE TABLE pfor_bad (c Array(String) CODEC(PFor('delta'))) ENGINE = MergeTree ORDER BY tuple(); -- { serverError BAD_ARGUMENTS }
CREATE TABLE pfor_bad (c FixedString(9) CODEC(PFor('delta'))) ENGINE = MergeTree ORDER BY tuple(); -- { serverError BAD_ARGUMENTS }
CREATE TABLE pfor_bad (c UUID CODEC(PFor('delta'))) ENGINE = MergeTree ORDER BY tuple(); -- { serverError BAD_ARGUMENTS }
CREATE TABLE pfor_bad (c UInt64 CODEC(PFor('bogus'))) ENGINE = MergeTree ORDER BY tuple(); -- { serverError ILLEGAL_CODEC_PARAMETER }
CREATE TABLE pfor_bad (c UInt64 CODEC(PFor(8))) ENGINE = MergeTree ORDER BY tuple(); -- { serverError ILLEGAL_CODEC_PARAMETER }
CREATE TABLE pfor_bad (c UInt64 CODEC(PFor('delta', 8))) ENGINE = MergeTree ORDER BY tuple(); -- { serverError ILLEGAL_SYNTAX_FOR_CODEC_TYPE }

-- A compressed block size that is not a multiple of the element size leaves a few bytes that do not form a whole
-- value; they are stored as is (the randomized `max_compress_block_size` of the test harness exercises this too).
CREATE TABLE pfor_unaligned (k UInt64, v UInt64 CODEC(PFor('double_delta'))) ENGINE = MergeTree ORDER BY k
SETTINGS min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0, index_granularity = 100, min_compress_block_size = 1001, max_compress_block_size = 1001;
INSERT INTO pfor_unaligned SELECT number, number * 15000 FROM numbers(10000);
SELECT 'unaligned blocks differ', count() FROM pfor_unaligned WHERE v != k * 15000;
DROP TABLE pfor_unaligned;

-- The codec is in beta and gated by a setting.
SET enable_pfor_codec = 0;
CREATE TABLE pfor_bad (c UInt64 CODEC(PFor('delta'))) ENGINE = MergeTree ORDER BY tuple(); -- { serverError BAD_ARGUMENTS }
SELECT count() FROM pfor_dd WHERE key < 10;

DROP TABLE pfor_ref;
DROP TABLE pfor_none;
DROP TABLE pfor_delta;
DROP TABLE pfor_dd;
DROP TABLE pfor_ts;
DROP TABLE pfor_desc;
