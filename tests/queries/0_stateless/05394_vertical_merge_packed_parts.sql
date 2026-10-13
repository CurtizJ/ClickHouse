-- Vertical merge into a Wide part with Packed storage, where all files of the part are buffered
-- in one in-memory archive and the per-column writers add their files next to the horizontal writer.

DROP TABLE IF EXISTS t_vertical_packed_src;
DROP TABLE IF EXISTS t_vertical_packed;

CREATE TABLE t_vertical_packed_src
(
    id UInt64,
    s String,
    n Nullable(UInt64),
    lc LowCardinality(String),
    arr Array(UInt32),
    nested Array(Array(String)),
    t Tuple(a UInt64, b String),
    m Map(String, UInt64)
)
ENGINE = Memory;

INSERT INTO t_vertical_packed_src SELECT
    number,
    toString(number),
    if(number % 7 = 0, NULL, number),
    concat('lc_', toString(number % 10)),
    range(number % 5),
    [[toString(number)], [], ['x', 'y']],
    (number * 3, toString(number * 5)),
    map(toString(number % 3), number)
FROM numbers(30000);

CREATE TABLE t_vertical_packed AS t_vertical_packed_src
ENGINE = MergeTree
ORDER BY id
SETTINGS min_bytes_for_wide_part = 0,
         min_rows_for_wide_part = 0,
         min_bytes_for_full_part_storage = '10G',
         enable_vertical_merge_algorithm = 1,
         vertical_merge_algorithm_min_rows_to_activate = 1,
         vertical_merge_algorithm_min_columns_to_activate = 1,
         vertical_merge_algorithm_min_bytes_to_activate = 0;

ALTER TABLE t_vertical_packed ADD INDEX idx_s s TYPE bloom_filter GRANULARITY 1;

INSERT INTO t_vertical_packed SELECT * FROM t_vertical_packed_src WHERE id % 3 = 0;
INSERT INTO t_vertical_packed SELECT * FROM t_vertical_packed_src WHERE id % 3 = 1;
INSERT INTO t_vertical_packed SELECT * FROM t_vertical_packed_src WHERE id % 3 = 2;

OPTIMIZE TABLE t_vertical_packed FINAL;
SYSTEM FLUSH LOGS part_log;

SELECT 'merge_algorithm', merge_algorithm FROM system.part_log
WHERE database = currentDatabase() AND table = 't_vertical_packed' AND event_type = 'MergeParts'
ORDER BY event_time_microseconds DESC LIMIT 1;

SELECT 'parts', count(), any(part_type), any(part_storage_type) FROM system.parts
WHERE database = currentDatabase() AND table = 't_vertical_packed' AND active;

SELECT 'same_data',
    (SELECT count(), sum(cityHash64(*)) FROM t_vertical_packed) = (SELECT count(), sum(cityHash64(*)) FROM t_vertical_packed_src);

SELECT 'skip_index', count() FROM t_vertical_packed WHERE s = '777';

CHECK TABLE t_vertical_packed SETTINGS check_query_single_value_result = 1;

DETACH TABLE t_vertical_packed;
ATTACH TABLE t_vertical_packed;

SELECT 'same_data_after_attach',
    (SELECT count(), sum(cityHash64(*)) FROM t_vertical_packed) = (SELECT count(), sum(cityHash64(*)) FROM t_vertical_packed_src);

DROP TABLE t_vertical_packed;
DROP TABLE t_vertical_packed_src;
