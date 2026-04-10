-- Tags: no-fasttest
-- Test text index with segmented posting lists (low segment_size for testing).
-- Verifies correct behavior when posting lists span multiple segments.

DROP TABLE IF EXISTS t_text_index_segments;

CREATE TABLE t_text_index_segments
(
    id UInt64,
    text String,
    INDEX idx_text text TYPE text(tokenizer = splitByNonAlpha, segment_size = 5) GRANULARITY 1
)
ENGINE = MergeTree
ORDER BY id
SETTINGS index_granularity = 4;

-- Insert 20 rows so posting lists span 4 segments (segment_size = 5).
-- Token 'hello' appears in rows 0,2,4,6,8,10,12,14,16,18 (across all segments).
-- Token 'world' appears in rows 1,3,5,7,9,11,13,15,17,19 (across all segments).
-- Token 'common' appears in all 20 rows.
INSERT INTO t_text_index_segments SELECT
    number,
    if(number % 2 = 0, 'hello common', 'world common')
FROM numbers(20);

SELECT 'hasToken hello';
SELECT id FROM t_text_index_segments WHERE hasToken(text, 'hello') ORDER BY id;

SELECT 'hasToken world';
SELECT id FROM t_text_index_segments WHERE hasToken(text, 'world') ORDER BY id;

SELECT 'hasToken common';
SELECT count() FROM t_text_index_segments WHERE hasToken(text, 'common');

SELECT 'hasAllTokens';
SELECT id FROM t_text_index_segments WHERE hasAllTokens(text, 'hello common') ORDER BY id;

SELECT 'hasAnyToken';
SELECT count() FROM t_text_index_segments WHERE hasAnyToken(text, 'hello world');

-- Test that text index is actually used (not just full scan).
SELECT 'index granules skipped';
SELECT count() FROM t_text_index_segments WHERE hasToken(text, 'hello')
SETTINGS force_data_skipping_indices = 'idx_text';

-- Test merge: insert another part and merge, result spans more segments.
INSERT INTO t_text_index_segments SELECT
    number + 20,
    if(number % 2 = 0, 'hello common', 'world common')
FROM numbers(20);

OPTIMIZE TABLE t_text_index_segments FINAL;

SELECT 'after merge hasToken hello';
SELECT count() FROM t_text_index_segments WHERE hasToken(text, 'hello');

SELECT 'after merge hasToken world';
SELECT count() FROM t_text_index_segments WHERE hasToken(text, 'world');

SELECT 'after merge hasAllTokens';
SELECT count() FROM t_text_index_segments WHERE hasAllTokens(text, 'hello common');

SELECT 'after merge forced index';
SELECT count() FROM t_text_index_segments WHERE hasToken(text, 'hello')
SETTINGS force_data_skipping_indices = 'idx_text';

DROP TABLE t_text_index_segments;
