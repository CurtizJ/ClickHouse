-- Tags: no-fasttest
-- Tag no-fasttest: depends on libstemmer_c

-- The text index postprocessor processes token batches in dictionary-encoded form when the
-- share of distinct tokens in the batch is low (the common case for natural text), executing
-- the expression once per unique token. Small inserts with mostly distinct tokens take the
-- flat fallback path instead, so the other postprocessor tests do not cover the
-- dictionary-encoded path. This test uses a large repetitive corpus to cover it end-to-end:
-- index build, stored dictionary content, and needle transform on search.

DROP TABLE IF EXISTS tab;

CREATE TABLE tab
(
    id UInt64,
    val String,
    INDEX idx(val) TYPE text(tokenizer = 'splitByNonAlpha', postprocessor = stem(lower(val), 'en'))
)
ENGINE = MergeTree ORDER BY id;

-- 10k rows with 3 words each drawn from a tiny vocabulary: ~30k occurrences, 11 distinct tokens.
INSERT INTO tab SELECT number, arrayStringConcat(array(
    ['running', 'studying', 'collection', 'walking', 'books'][1 + number % 5],
    ['runs', 'studied', 'collects', 'walked', 'book'][1 + intDiv(number, 5) % 5],
    'CONSTANT'), ' ')
FROM numbers(10000);

-- The dictionary must contain exactly the stemmed forms - not the original words.
SELECT token, cardinality FROM mergeTreeTextIndex(currentDatabase(), tab, idx) ORDER BY token;

-- Needles are stemmed before the index lookup, so any morphological form matches.
SELECT count() FROM tab WHERE hasToken(val, 'run');       -- 'running' in word1 or 'runs' in word2
SELECT count() FROM tab WHERE hasToken(val, 'study');     -- 'studying' in word1 or 'studied' in word2
SELECT count() FROM tab WHERE hasToken(val, 'constant');  -- lower of 'CONSTANT'; all rows
SELECT count() FROM tab WHERE hasToken(val, 'jumping');   -- 0
SELECT count() FROM tab WHERE hasAllTokens(val, ['running', 'studied']);

DROP TABLE tab;
