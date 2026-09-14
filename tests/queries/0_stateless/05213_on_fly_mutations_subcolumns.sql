-- Tags: no-shared-catalog
-- no-shared-catalog: STOP MERGES only stops them on the current replica, the second one can
-- materialize the mutations this test needs to stay pending

-- A subcolumn of a column rewritten by a pending mutation applied on the fly is extracted from the
-- rewritten column, exactly like a read of the whole column, and not from the stale data of the part.
-- Every section reads the same queries while the mutations are pending and after they are materialized.

SET apply_mutations_on_fly = 1;
SET mutations_sync = 0;
SET optimize_functions_to_subcolumns = 1;

SELECT 'array, wide part: on the fly';

DROP TABLE IF EXISTS t_on_fly_subcolumn_array;

CREATE TABLE t_on_fly_subcolumn_array (id UInt64, arr Array(UInt64))
ENGINE = MergeTree ORDER BY id
SETTINGS min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0;

INSERT INTO t_on_fly_subcolumn_array VALUES (1, [1]), (2, [2, 2]), (3, [3, 3, 3]);

SYSTEM STOP MERGES t_on_fly_subcolumn_array;

ALTER TABLE t_on_fly_subcolumn_array UPDATE arr = [9, 9, 9, 9] WHERE id = 2;
ALTER TABLE t_on_fly_subcolumn_array UPDATE arr = [8, 8, 8, 8, 8] WHERE id = 3;

SELECT id, arr, arr.size0 FROM t_on_fly_subcolumn_array ORDER BY id;
SELECT id, arr.size0 FROM t_on_fly_subcolumn_array ORDER BY id;
SELECT id, length(arr) FROM t_on_fly_subcolumn_array ORDER BY id;
SELECT count() FROM t_on_fly_subcolumn_array WHERE arr.size0 = 4;
SELECT id FROM t_on_fly_subcolumn_array PREWHERE arr.size0 = 4;
SELECT id, arr FROM t_on_fly_subcolumn_array PREWHERE arr.size0 > 1 WHERE length(arr) < 5 ORDER BY id;

SYSTEM START MERGES t_on_fly_subcolumn_array;
ALTER TABLE t_on_fly_subcolumn_array UPDATE arr = arr WHERE 1 SETTINGS mutations_sync = 2;

SELECT 'array, wide part: materialized';

SELECT id, arr, arr.size0 FROM t_on_fly_subcolumn_array ORDER BY id;
SELECT id, arr.size0 FROM t_on_fly_subcolumn_array ORDER BY id;
SELECT id, length(arr) FROM t_on_fly_subcolumn_array ORDER BY id;
SELECT count() FROM t_on_fly_subcolumn_array WHERE arr.size0 = 4;
SELECT id FROM t_on_fly_subcolumn_array PREWHERE arr.size0 = 4;
SELECT id, arr FROM t_on_fly_subcolumn_array PREWHERE arr.size0 > 1 WHERE length(arr) < 5 ORDER BY id;

DROP TABLE t_on_fly_subcolumn_array;

-- A later mutation reads a subcolumn of the column an earlier mutation rewrote: the step of the
-- later mutation has to take it from the output of the earlier step.
SELECT 'subcolumn read by a later mutation: on the fly';

DROP TABLE IF EXISTS t_on_fly_subcolumn_chain;

CREATE TABLE t_on_fly_subcolumn_chain (id UInt64, arr Array(UInt64), n UInt64)
ENGINE = MergeTree ORDER BY id
SETTINGS min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0;

INSERT INTO t_on_fly_subcolumn_chain VALUES (1, [1], 0), (2, [2, 2], 0), (3, [3, 3, 3], 0);

SYSTEM STOP MERGES t_on_fly_subcolumn_chain;

ALTER TABLE t_on_fly_subcolumn_chain UPDATE arr = [9, 9, 9, 9] WHERE id = 2;
ALTER TABLE t_on_fly_subcolumn_chain UPDATE n = arr.size0 WHERE 1;

SELECT id, n FROM t_on_fly_subcolumn_chain ORDER BY id;
SELECT id, arr, n, arr.size0 FROM t_on_fly_subcolumn_chain ORDER BY id;

SYSTEM START MERGES t_on_fly_subcolumn_chain;
ALTER TABLE t_on_fly_subcolumn_chain UPDATE n = n WHERE 1 SETTINGS mutations_sync = 2;

SELECT 'subcolumn read by a later mutation: materialized';

SELECT id, n FROM t_on_fly_subcolumn_chain ORDER BY id;
SELECT id, arr, n, arr.size0 FROM t_on_fly_subcolumn_chain ORDER BY id;

DROP TABLE t_on_fly_subcolumn_chain;

-- An earlier mutation reads a subcolumn from the part, a later one rewrites the parent: the delete
-- sees the sizes before the update (only the row with three elements is deleted), and the query
-- sees the size after it.
SELECT 'subcolumn read by an earlier mutation: on the fly';

DROP TABLE IF EXISTS t_on_fly_subcolumn_delete;

CREATE TABLE t_on_fly_subcolumn_delete (id UInt64, arr Array(UInt64))
ENGINE = MergeTree ORDER BY id
SETTINGS min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0;

INSERT INTO t_on_fly_subcolumn_delete VALUES (1, [1]), (2, [2, 2]), (3, [3, 3, 3]);

SYSTEM STOP MERGES t_on_fly_subcolumn_delete;

ALTER TABLE t_on_fly_subcolumn_delete DELETE WHERE arr.size0 > 2;
ALTER TABLE t_on_fly_subcolumn_delete UPDATE arr = [9, 9, 9, 9] WHERE id = 2;

SELECT id, arr, arr.size0 FROM t_on_fly_subcolumn_delete ORDER BY id;
SELECT id, arr.size0 FROM t_on_fly_subcolumn_delete ORDER BY id;

SYSTEM START MERGES t_on_fly_subcolumn_delete;
ALTER TABLE t_on_fly_subcolumn_delete UPDATE arr = arr WHERE 1 SETTINGS mutations_sync = 2;

SELECT 'subcolumn read by an earlier mutation: materialized';

SELECT id, arr, arr.size0 FROM t_on_fly_subcolumn_delete ORDER BY id;
SELECT id, arr.size0 FROM t_on_fly_subcolumn_delete ORDER BY id;

DROP TABLE t_on_fly_subcolumn_delete;

SELECT 'tuple, compact part: on the fly';

DROP TABLE IF EXISTS t_on_fly_subcolumn_tuple;

CREATE TABLE t_on_fly_subcolumn_tuple (id UInt64, t Tuple(a UInt64, b String))
ENGINE = MergeTree ORDER BY id
SETTINGS min_bytes_for_wide_part = '1G', min_rows_for_wide_part = 1000000;

INSERT INTO t_on_fly_subcolumn_tuple VALUES (1, (1, 'x')), (2, (2, 'y'));

SYSTEM STOP MERGES t_on_fly_subcolumn_tuple;

ALTER TABLE t_on_fly_subcolumn_tuple UPDATE t = (100, 'z') WHERE id = 2;

SELECT id, t.a, t.b FROM t_on_fly_subcolumn_tuple ORDER BY id;
SELECT id, tupleElement(t, 'a') FROM t_on_fly_subcolumn_tuple ORDER BY id;
SELECT count() FROM t_on_fly_subcolumn_tuple WHERE t.a = 100;
SELECT id FROM t_on_fly_subcolumn_tuple PREWHERE t.b = 'z';

SYSTEM START MERGES t_on_fly_subcolumn_tuple;
ALTER TABLE t_on_fly_subcolumn_tuple UPDATE t = t WHERE 1 SETTINGS mutations_sync = 2;

SELECT 'tuple, compact part: materialized';

SELECT id, t.a, t.b FROM t_on_fly_subcolumn_tuple ORDER BY id;
SELECT id, tupleElement(t, 'a') FROM t_on_fly_subcolumn_tuple ORDER BY id;
SELECT count() FROM t_on_fly_subcolumn_tuple WHERE t.a = 100;
SELECT id FROM t_on_fly_subcolumn_tuple PREWHERE t.b = 'z';

DROP TABLE t_on_fly_subcolumn_tuple;

SELECT 'nullable and map: on the fly';

DROP TABLE IF EXISTS t_on_fly_subcolumn_null_map;

CREATE TABLE t_on_fly_subcolumn_null_map (id UInt64, x Nullable(UInt64), m Map(String, UInt64))
ENGINE = MergeTree ORDER BY id
SETTINGS min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0;

INSERT INTO t_on_fly_subcolumn_null_map VALUES (1, 1, map('a', 1)), (2, NULL, map());

SYSTEM STOP MERGES t_on_fly_subcolumn_null_map;

ALTER TABLE t_on_fly_subcolumn_null_map UPDATE x = NULL, m = map() WHERE id = 1;
ALTER TABLE t_on_fly_subcolumn_null_map UPDATE x = 5, m = map('k1', 1, 'k2', 2) WHERE id = 2;

SELECT id, x, x.null, isNull(x), m, m.keys, mapKeys(m), m.values, length(m) FROM t_on_fly_subcolumn_null_map ORDER BY id;
SELECT id FROM t_on_fly_subcolumn_null_map WHERE x IS NULL;
SELECT id FROM t_on_fly_subcolumn_null_map WHERE has(mapKeys(m), 'k2');

SYSTEM START MERGES t_on_fly_subcolumn_null_map;
ALTER TABLE t_on_fly_subcolumn_null_map UPDATE x = x WHERE 1 SETTINGS mutations_sync = 2;

SELECT 'nullable and map: materialized';

SELECT id, x, x.null, isNull(x), m, m.keys, mapKeys(m), m.values, length(m) FROM t_on_fly_subcolumn_null_map ORDER BY id;
SELECT id FROM t_on_fly_subcolumn_null_map WHERE x IS NULL;
SELECT id FROM t_on_fly_subcolumn_null_map WHERE has(mapKeys(m), 'k2');

DROP TABLE t_on_fly_subcolumn_null_map;

-- One mutation with two commands: the second reads a subcolumn of the column the first rewrites.
SELECT 'two commands in one mutation';

DROP TABLE IF EXISTS t_on_fly_subcolumn_two_commands;

CREATE TABLE t_on_fly_subcolumn_two_commands (id UInt64, arr Array(UInt64), n UInt64)
ENGINE = MergeTree ORDER BY id
SETTINGS min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0;

INSERT INTO t_on_fly_subcolumn_two_commands VALUES (1, [1], 0), (2, [2, 2], 0), (3, [3, 3, 3], 0);

ALTER TABLE t_on_fly_subcolumn_two_commands UPDATE arr = [9, 9, 9, 9] WHERE id = 2, UPDATE n = arr.size0 WHERE 1 SETTINGS mutations_sync = 2;

SELECT id, arr, n FROM t_on_fly_subcolumn_two_commands ORDER BY id;

DROP TABLE t_on_fly_subcolumn_two_commands;
