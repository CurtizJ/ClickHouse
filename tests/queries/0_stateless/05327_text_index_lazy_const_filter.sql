-- The lazy text index reader returns a constant zero column for a read without matching rows,
-- and the filter drops such rows without scanning them. Marks outside the row range of a token's
-- posting-list segments are not read, so the reads without matches below are inside the row range
-- of a segment straddling a gap of the token. The primary key conditions split one block into several
-- mark ranges, so constant and full results of the separate reads are appended into the same column
-- in every order. Every lazy result is compared with a plain column scan.

SET enable_full_text_index = 1;
SET use_skip_indexes_on_data_read = 1;
SET query_plan_optimize_count_from_text_index = 0;
SET use_query_condition_cache = 0;
SET merge_tree_read_split_ranges_into_intersecting_and_non_intersecting_injection_probability = 0;
SET max_threads = 1;
SET max_block_size = 100000;

DROP TABLE IF EXISTS tab_lazy_const;

CREATE TABLE tab_lazy_const
(
    id UInt64,
    s String,
    INDEX idx s TYPE text(tokenizer = splitByNonAlpha, posting_list_codec = 'bitpacking', posting_list_block_size = 1024) GRANULARITY 100000000
)
ENGINE = MergeTree ORDER BY id
SETTINGS index_granularity = 1024, index_granularity_bytes = '10Mi';

--   gapped : every row in [0, 1500), [30000, 31500) and [60000, 63000) -> 6000 postings in segments of 1024:
--            the second segment spans the rows [1024, 30547], the third one spans the rows [30548, 60071]
--   small  : every 4th row in [30000, 30400) -> 100 postings, read by the index analysis
--   eighth : every 8th row
INSERT INTO tab_lazy_const
SELECT number,
    concat('base',
        if(number < 1500 OR (number >= 30000 AND number < 31500) OR (number >= 60000 AND number < 63000), ' gapped', ''),
        if(number >= 30000 AND number < 30400 AND number % 4 = 0, ' small', ''),
        if(number % 8 = 0, ' eighth', ''))
FROM numbers(100000)
SETTINGS max_insert_threads = 1, max_insert_block_size = 1000000, min_insert_block_size_rows = 1000000, min_insert_block_size_bytes = 0;

SELECT 'parts', count() FROM system.parts WHERE database = currentDatabase() AND table = 'tab_lazy_const' AND active;

SELECT 'Ground truth (no index)';
SET use_skip_indexes = 0;
SET query_plan_direct_read_from_text_index = 0;
SELECT 'const, full', count(), sum(id) FROM tab_lazy_const WHERE (id >= 10240 AND id < 20480 OR id >= 29696 AND id < 31744) AND hasToken(s, 'gapped');
SELECT 'full, const', count(), sum(id) FROM tab_lazy_const WHERE (id < 1024 OR id >= 10240 AND id < 20480) AND hasToken(s, 'gapped');
SELECT 'const, full, const', count(), sum(id) FROM tab_lazy_const WHERE (id >= 10240 AND id < 20480 OR id >= 29696 AND id < 31744 OR id >= 40960 AND id < 51200) AND hasToken(s, 'gapped');
SELECT 'full, const, full', count(), sum(id) FROM tab_lazy_const WHERE (id < 1024 OR id >= 10240 AND id < 20480 OR id >= 29696 AND id < 31744) AND hasToken(s, 'gapped');
SELECT 'const, const', count(), sum(id) FROM tab_lazy_const WHERE (id >= 10240 AND id < 20480 OR id >= 40960 AND id < 51200) AND hasToken(s, 'gapped');
SELECT 'all, const, full', count(), sum(id) FROM tab_lazy_const WHERE (id >= 10240 AND id < 20480 OR id >= 29696 AND id < 31744) AND hasAllTokens(s, ['gapped', 'eighth']);
SELECT 'all, const, const', count(), sum(id) FROM tab_lazy_const WHERE (id >= 10240 AND id < 20480 OR id >= 40960 AND id < 51200) AND hasAllTokens(s, ['gapped', 'eighth']);
SELECT 'any, const, full', count(), sum(id) FROM tab_lazy_const WHERE (id >= 10240 AND id < 20480 OR id >= 29696 AND id < 31744) AND hasAnyTokens(s, ['gapped', 'small']);
SELECT 'in expression', count(), sum(id), countIf(hasToken(s, 'gapped')) FROM tab_lazy_const WHERE (id >= 10240 AND id < 20480 OR id >= 29696 AND id < 31744) AND (hasToken(s, 'gapped') OR id % 1000 = 0);
SELECT 'small blocks', count(), sum(id) FROM tab_lazy_const WHERE hasToken(s, 'gapped') SETTINGS max_block_size = 4096;
SELECT 'small blocks, all', count(), sum(id) FROM tab_lazy_const WHERE hasAllTokens(s, ['gapped', 'eighth']) SETTINGS max_block_size = 4096;

SET use_skip_indexes = 1;
SET query_plan_direct_read_from_text_index = 1;
SET text_index_posting_list_apply_mode = 'lazy';

SELECT 'Lazy, leapfrog intersection';
SET text_index_postings_intersection_algorithm = 'leapfrog';
SELECT 'const, full', count(), sum(id) FROM tab_lazy_const WHERE (id >= 10240 AND id < 20480 OR id >= 29696 AND id < 31744) AND hasToken(s, 'gapped');
SELECT 'full, const', count(), sum(id) FROM tab_lazy_const WHERE (id < 1024 OR id >= 10240 AND id < 20480) AND hasToken(s, 'gapped');
SELECT 'const, full, const', count(), sum(id) FROM tab_lazy_const WHERE (id >= 10240 AND id < 20480 OR id >= 29696 AND id < 31744 OR id >= 40960 AND id < 51200) AND hasToken(s, 'gapped');
SELECT 'full, const, full', count(), sum(id) FROM tab_lazy_const WHERE (id < 1024 OR id >= 10240 AND id < 20480 OR id >= 29696 AND id < 31744) AND hasToken(s, 'gapped');
SELECT 'const, const', count(), sum(id) FROM tab_lazy_const WHERE (id >= 10240 AND id < 20480 OR id >= 40960 AND id < 51200) AND hasToken(s, 'gapped');
SELECT 'all, const, full', count(), sum(id) FROM tab_lazy_const WHERE (id >= 10240 AND id < 20480 OR id >= 29696 AND id < 31744) AND hasAllTokens(s, ['gapped', 'eighth']);
SELECT 'all, const, const', count(), sum(id) FROM tab_lazy_const WHERE (id >= 10240 AND id < 20480 OR id >= 40960 AND id < 51200) AND hasAllTokens(s, ['gapped', 'eighth']);
SELECT 'any, const, full', count(), sum(id) FROM tab_lazy_const WHERE (id >= 10240 AND id < 20480 OR id >= 29696 AND id < 31744) AND hasAnyTokens(s, ['gapped', 'small']);
SELECT 'in expression', count(), sum(id), countIf(hasToken(s, 'gapped')) FROM tab_lazy_const WHERE (id >= 10240 AND id < 20480 OR id >= 29696 AND id < 31744) AND (hasToken(s, 'gapped') OR id % 1000 = 0);
SELECT 'small blocks', count(), sum(id) FROM tab_lazy_const WHERE hasToken(s, 'gapped') SETTINGS max_block_size = 4096;
SELECT 'small blocks, all', count(), sum(id) FROM tab_lazy_const WHERE hasAllTokens(s, ['gapped', 'eighth']) SETTINGS max_block_size = 4096;

SELECT 'Lazy, brute-force intersection';
SET text_index_postings_intersection_algorithm = 'bruteforce';
SELECT 'all, const, full', count(), sum(id) FROM tab_lazy_const WHERE (id >= 10240 AND id < 20480 OR id >= 29696 AND id < 31744) AND hasAllTokens(s, ['gapped', 'eighth']);
SELECT 'all, const, const', count(), sum(id) FROM tab_lazy_const WHERE (id >= 10240 AND id < 20480 OR id >= 40960 AND id < 51200) AND hasAllTokens(s, ['gapped', 'eighth']);
SELECT 'small blocks, all', count(), sum(id) FROM tab_lazy_const WHERE hasAllTokens(s, ['gapped', 'eighth']) SETTINGS max_block_size = 4096;

DROP TABLE tab_lazy_const;
