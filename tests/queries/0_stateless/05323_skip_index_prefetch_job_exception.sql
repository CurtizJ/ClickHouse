-- Tags: no-parallel
-- - no-parallel -- enables a failpoint

-- An exception in a skip index prefetch job fails the query with that exception, whether the job ran
-- on the prefetch pool or was claimed and run inline by the analysis, and the next query works.

DROP TABLE IF EXISTS t_skip_index_prefetch_exception;

CREATE TABLE t_skip_index_prefetch_exception
(
    id UInt64,
    v UInt64,
    INDEX idx_v v TYPE minmax GRANULARITY 1
)
ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 256, index_granularity_bytes = '100Mi', max_bytes_to_merge_at_max_space_in_pool = 1;

INSERT INTO t_skip_index_prefetch_exception SELECT number, number FROM numbers(10000);
INSERT INTO t_skip_index_prefetch_exception SELECT number, number FROM numbers(10000, 10000);

SET use_skip_indexes = 1, use_skip_indexes_prefetch = 1, use_skip_indexes_on_data_read = 0, use_query_condition_cache = 0;
SET max_rows_to_read = 0, enable_parallel_replicas = 0, use_reader_executor = 0;
SET local_filesystem_read_method = 'pread_threadpool', local_filesystem_read_prefetch = 1;
SET remote_filesystem_read_method = 'threadpool', remote_filesystem_read_prefetch = 1;

SYSTEM ENABLE FAILPOINT skip_index_prefetch_job_exception;

SELECT count() FROM t_skip_index_prefetch_exception WHERE v >= 15000 SETTINGS max_threads_for_indexes = 1; -- { serverError FAULT_INJECTED }
SELECT count() FROM t_skip_index_prefetch_exception WHERE v >= 15000 SETTINGS max_threads_for_indexes = 2; -- { serverError FAULT_INJECTED }

SYSTEM DISABLE FAILPOINT skip_index_prefetch_job_exception;

SELECT count() FROM t_skip_index_prefetch_exception WHERE v >= 15000;

DROP TABLE t_skip_index_prefetch_exception;
