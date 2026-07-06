-- Test for in-memory compression of the primary key.
-- Columns of unsigned integers and strings are kept bit-packed in memory
-- if it takes less memory than the raw columns.

DROP TABLE IF EXISTS t_pk_compressed;

CREATE TABLE t_pk_compressed (u UInt64, s String)
ENGINE = MergeTree ORDER BY (u, s)
SETTINGS
    index_granularity = 1,
    index_granularity_bytes = '10M',
    use_primary_key_cache = 0,
    primary_key_lazy_load = 0,
    primary_key_ratio_of_unique_prefix_values_to_skip_suffix_columns = 1.0;

INSERT INTO t_pk_compressed
SELECT number, concat('str_', leftPad(toString(number), 7, '0'))
FROM numbers(30000);

SELECT '-- compressed index takes less memory than raw columns (raw is at least 29 bytes per mark)';
SELECT primary_key_bytes_in_memory > 0, primary_key_bytes_in_memory < 20 * 30000, primary_key_bytes_in_memory_allocated < 20 * 30000
FROM system.parts WHERE database = currentDatabase() AND table = 't_pk_compressed' AND active;

SELECT '-- point queries';
SELECT count() FROM t_pk_compressed WHERE u = 12345 SETTINGS max_rows_to_read = 4;
SELECT count() FROM t_pk_compressed WHERE u = 12345 AND s = 'str_0012345' SETTINGS max_rows_to_read = 4;
SELECT u, s FROM t_pk_compressed WHERE u IN (0, 999, 29999) ORDER BY u SETTINGS max_rows_to_read = 12;

SELECT '-- the same queries with the lightweight index analysis';
SELECT count() FROM t_pk_compressed WHERE u = 12345 SETTINGS max_rows_to_read = 4, use_lightweight_primary_key_index_analysis = 1;
SELECT count() FROM t_pk_compressed WHERE u = 12345 AND s = 'str_0012345' SETTINGS max_rows_to_read = 4, use_lightweight_primary_key_index_analysis = 1;

SELECT '-- and without it';
SELECT count() FROM t_pk_compressed WHERE u = 12345 SETTINGS max_rows_to_read = 4, use_lightweight_primary_key_index_analysis = 0;
SELECT count() FROM t_pk_compressed WHERE u = 12345 AND s = 'str_0012345' SETTINGS max_rows_to_read = 4, use_lightweight_primary_key_index_analysis = 0;

SELECT '-- queries with a chain of monotonic functions';
SELECT count() FROM t_pk_compressed WHERE u + 10 = 12355 SETTINGS max_rows_to_read = 4;
SELECT count() FROM t_pk_compressed WHERE toUInt32(u) = 12345 SETTINGS max_rows_to_read = 4;

SELECT '-- the index is decompressed for the mergeTreeIndex table function';
SELECT u, s FROM mergeTreeIndex(currentDatabase(), t_pk_compressed) ORDER BY mark_number LIMIT 3;
SELECT sum(u != mark_number) <= 1 FROM mergeTreeIndex(currentDatabase(), t_pk_compressed);

SELECT '-- the index loaded from disk is compressed as well';
DETACH TABLE t_pk_compressed;
ATTACH TABLE t_pk_compressed;

SELECT count() FROM t_pk_compressed WHERE u = 12345 SETTINGS max_rows_to_read = 4;
SELECT count() FROM t_pk_compressed WHERE u = 12345 AND s = 'str_0012345' SETTINGS max_rows_to_read = 4;

SELECT primary_key_bytes_in_memory > 0, primary_key_bytes_in_memory < 20 * 30000, primary_key_bytes_in_memory_allocated < 20 * 30000
FROM system.parts WHERE database = currentDatabase() AND table = 't_pk_compressed' AND active;

DROP TABLE t_pk_compressed;

-- The same with a String column as the first key column.
DROP TABLE IF EXISTS t_pk_compressed_str;

CREATE TABLE t_pk_compressed_str (s String, d Date)
ENGINE = MergeTree ORDER BY (s, d)
SETTINGS
    index_granularity = 1,
    index_granularity_bytes = '10M',
    use_primary_key_cache = 0,
    primary_key_lazy_load = 0,
    primary_key_ratio_of_unique_prefix_values_to_skip_suffix_columns = 1.0;

INSERT INTO t_pk_compressed_str
SELECT concat('str_', leftPad(toString(number), 7, '0')), toDate('2024-01-01') + intDiv(number, 1000)
FROM numbers(30000);

SELECT '-- point queries by the String key column';
SELECT count() FROM t_pk_compressed_str WHERE s = 'str_0012345' SETTINGS max_rows_to_read = 4;
SELECT count() FROM t_pk_compressed_str WHERE s = 'str_0012345' SETTINGS max_rows_to_read = 4, use_lightweight_primary_key_index_analysis = 1;
SELECT count() FROM t_pk_compressed_str WHERE s = 'str_0012345' SETTINGS max_rows_to_read = 4, use_lightweight_primary_key_index_analysis = 0;
SELECT count() FROM t_pk_compressed_str WHERE s >= 'str_0029000' SETTINGS max_rows_to_read = 1002;
SELECT s, d FROM mergeTreeIndex(currentDatabase(), t_pk_compressed_str) ORDER BY mark_number LIMIT 3;

SELECT primary_key_bytes_in_memory > 0, primary_key_bytes_in_memory < 20 * 30000
FROM system.parts WHERE database = currentDatabase() AND table = 't_pk_compressed_str' AND active;

DROP TABLE t_pk_compressed_str;

-- Incompressible and nullable columns are kept raw, the queries must work the same.
DROP TABLE IF EXISTS t_pk_raw;

CREATE TABLE t_pk_raw (u UInt64, n Nullable(UInt64))
ENGINE = MergeTree ORDER BY (u, n)
SETTINGS
    allow_nullable_key = 1,
    index_granularity = 1,
    index_granularity_bytes = '10M',
    use_primary_key_cache = 0,
    primary_key_lazy_load = 0,
    primary_key_ratio_of_unique_prefix_values_to_skip_suffix_columns = 1.0;

INSERT INTO t_pk_raw SELECT sipHash64(number), if(number % 3 = 0, NULL, number) FROM numbers(10000);

SELECT '-- queries over incompressible and nullable key columns';
SELECT count() FROM t_pk_raw WHERE u = sipHash64(9998::UInt64) SETTINGS max_rows_to_read = 4;
SELECT count() FROM t_pk_raw WHERE n IS NULL SETTINGS use_lightweight_primary_key_index_analysis = 1;
SELECT count() FROM t_pk_raw WHERE u = sipHash64(9999::UInt64) AND n IS NOT NULL SETTINGS max_rows_to_read = 4;

DROP TABLE t_pk_raw;
