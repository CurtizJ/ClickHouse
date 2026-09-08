SET enable_lightweight_update = 1, apply_patch_parts = 1, max_threads = 1;
SET max_block_size = 1024, merge_tree_min_read_task_size = 1000000;
SET log_queries = 1, log_queries_probability = 1, log_profile_events = 1;

DROP TABLE IF EXISTS t_lwu_equal_run;
CREATE TABLE t_lwu_equal_run (k UInt64, u UInt64, v UInt64, w UInt64)
ENGINE = MergeTree PRIMARY KEY k ORDER BY (k, u)
SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1,
    patch_parts_version = 'v2', index_granularity = 1024, index_granularity_bytes = 0;

-- Merge interleaved keys to ensure identities are not ordered within the shortened key.
INSERT INTO t_lwu_equal_run SELECT 0, number * 2, number * 2, 0 FROM numbers(25000);
INSERT INTO t_lwu_equal_run SELECT 0, number * 2 + 1, number * 2 + 1, 0 FROM numbers(25000);
OPTIMIZE TABLE t_lwu_equal_run FINAL;
SYSTEM STOP MERGES t_lwu_equal_run;
UPDATE t_lwu_equal_run SET v = u + 100000 WHERE 1;
UPDATE t_lwu_equal_run SET v = u + 200000, w = 10 WHERE u % 2 = 0;
UPDATE t_lwu_equal_run SET w = 20 WHERE u % 3 = 0;
ALTER TABLE t_lwu_equal_run MODIFY ORDER BY k;

SELECT sum(v = u + if(u % 2 = 0, 200000, 100000)),
    sum(w = if(u % 3 = 0, 20, if(u % 2 = 0, 10, 0)))
FROM t_lwu_equal_run SETTINGS log_comment = 'lwu_equal_run_reuse';

SYSTEM FLUSH LOGS query_log;
SELECT count() = 1, max(ProfileEvents['PatchesMergeOnKeyRunMapReuses']) > 10,
    max(ProfileEvents['PatchesMergeOnKeyRunMapRows']) < 200000
FROM system.query_log
WHERE type = 'QueryFinish' AND current_database = currentDatabase() AND log_comment = 'lwu_equal_run_reuse';

SELECT sum(v = u + if(u % 2 = 0, 200000, 100000)), sum(w = 20)
FROM t_lwu_equal_run PREWHERE u % 3 = 0;

SYSTEM START MERGES t_lwu_equal_run;
OPTIMIZE TABLE t_lwu_equal_run FINAL;
SELECT sum(v = u + if(u % 2 = 0, 200000, 100000)),
    sum(w = if(u % 3 = 0, 20, if(u % 2 = 0, 10, 0)))
FROM t_lwu_equal_run SETTINGS apply_patch_parts = 0;
DROP TABLE t_lwu_equal_run;

-- An empty sorting key is a single run, including across filtered result blocks.
CREATE TABLE t_lwu_equal_run (u UInt64, v UInt64)
ENGINE = MergeTree ORDER BY tuple()
SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1, patch_parts_version = 'v2';
SYSTEM STOP MERGES t_lwu_equal_run;
INSERT INTO t_lwu_equal_run SELECT number, 0 FROM numbers(50000);
UPDATE t_lwu_equal_run SET v = u + 1 WHERE 1;
SELECT sum(v) FROM t_lwu_equal_run;
SELECT sum(v) FROM t_lwu_equal_run PREWHERE u % 2 = 0;
DROP TABLE t_lwu_equal_run;

-- Advance across run boundaries and resident patch blocks, including disjoint ranges in descending order.
CREATE TABLE t_lwu_equal_run (k UInt64, u UInt64, v UInt64)
ENGINE = MergeTree ORDER BY k DESC
SETTINGS allow_experimental_reverse_key = 1, enable_block_number_column = 1,
    enable_block_offset_column = 1, patch_parts_version = 'v2', index_granularity = 1024;
SYSTEM STOP MERGES t_lwu_equal_run;
INSERT INTO t_lwu_equal_run SELECT intDiv(number, 20000), number, number FROM numbers(100000);
UPDATE t_lwu_equal_run SET v = u + 1 WHERE 1;
UPDATE t_lwu_equal_run SET v = u + 2 WHERE u % 3 = 0;
SELECT countIf(v != u + if(u % 3 = 0, 2, 1)) FROM t_lwu_equal_run;
SELECT countIf(v != u + if(u % 3 = 0, 2, 1)) FROM t_lwu_equal_run PREWHERE u % 7 = 0;
SELECT countIf(v != u + if(u % 3 = 0, 2, 1)) FROM t_lwu_equal_run
WHERE k IN (0, 2, 4) SETTINGS optimize_read_in_order = 0;
DROP TABLE t_lwu_equal_run;
