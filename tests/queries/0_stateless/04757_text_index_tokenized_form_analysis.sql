-- The 3-argument text-search form with an explicit tokenizer, e.g.
-- hasAnyTokens(s, ['a', 'b'], 'splitByNonAlpha'), must be usable by text index analysis when the
-- explicit tokenizer equals the index tokenizer (this is also the form the query-plan rewrite of
-- the 2-argument form produces, so plan fragments re-optimized on another node analyze it).
-- A mismatched tokenizer or an index with a pre/postprocessor must not match: the index cannot
-- answer for a foreign tokenizer, and pre/postprocessor transforms are baked into the indexed
-- tokens but not into the function's arguments.

SET enable_analyzer = 1;
SET use_skip_indexes = 1;
SET use_skip_indexes_on_data_read = 1;
SET query_plan_direct_read_from_text_index = 1;

DROP TABLE IF EXISTS t_tokenized_form;

CREATE TABLE t_tokenized_form (id UInt64, s String, INDEX idx s TYPE text(tokenizer = 'splitByNonAlpha') GRANULARITY 1)
ENGINE = MergeTree ORDER BY id SETTINGS index_granularity = 4;

INSERT INTO t_tokenized_form SELECT number, if(number = 13, 'hello world', 'foo bar ' || toString(number)) FROM numbers(32);

SELECT '-- result parity: 2-arg vs 3-arg';
SELECT count() FROM t_tokenized_form WHERE hasAnyTokens(s, ['hello']);
SELECT count() FROM t_tokenized_form WHERE hasAnyTokens(s, ['hello'], 'splitByNonAlpha');
SELECT count() FROM t_tokenized_form WHERE hasAllTokens(s, ['hello', 'world']);
SELECT count() FROM t_tokenized_form WHERE hasAllTokens(s, ['hello', 'world'], 'splitByNonAlpha');
SELECT count() FROM t_tokenized_form WHERE hasPhrase(s, 'hello world');
SELECT count() FROM t_tokenized_form WHERE hasPhrase(s, 'hello world', 'splitByNonAlpha');

SELECT '-- static granule pruning engages for the 3-arg form';
SELECT trim(explain) FROM (EXPLAIN indexes = 1 SELECT count() FROM t_tokenized_form WHERE hasAnyTokens(s, ['hello'], 'splitByNonAlpha'))
WHERE explain LIKE '%Granules:%' SETTINGS use_skip_indexes_on_data_read = 0;

SELECT '-- direct read engages for the 3-arg forms';
SELECT countIf(explain LIKE '%__text_index_%') > 0 FROM (EXPLAIN actions = 1 SELECT count() FROM t_tokenized_form WHERE hasAnyTokens(s, ['hello'], 'splitByNonAlpha'));
SELECT countIf(explain LIKE '%__text_index_%') > 0 FROM (EXPLAIN actions = 1 SELECT count() FROM t_tokenized_form WHERE hasAllTokens(s, ['hello', 'world'], 'splitByNonAlpha'));
SELECT countIf(explain LIKE '%__text_index_%') > 0 FROM (EXPLAIN actions = 1 SELECT count() FROM t_tokenized_form WHERE hasPhrase(s, 'hello world', 'splitByNonAlpha'));

SELECT '-- a mismatched tokenizer argument must not use the index';
SELECT countIf(explain LIKE '%__text_index_%') FROM (EXPLAIN actions = 1 SELECT count() FROM t_tokenized_form WHERE hasAnyTokens(s, ['hello'], 'ngrams(3)'));
SELECT count() FROM t_tokenized_form WHERE hasAnyTokens(s, ['hel'], 'ngrams(3)');

DROP TABLE t_tokenized_form;

SELECT '-- an index with a preprocessor must not match the 3-arg form (2-arg control does)';
DROP TABLE IF EXISTS t_tokenized_form_preproc;

CREATE TABLE t_tokenized_form_preproc (id UInt64, s String, INDEX idx s TYPE text(tokenizer = 'splitByNonAlpha', preprocessor = lower(s)) GRANULARITY 1)
ENGINE = MergeTree ORDER BY id SETTINGS index_granularity = 4;

INSERT INTO t_tokenized_form_preproc SELECT number, if(number = 13, 'HELLO world', 'foo bar ' || toString(number)) FROM numbers(32);

SELECT countIf(explain LIKE '%__text_index_%') FROM (EXPLAIN actions = 1 SELECT count() FROM t_tokenized_form_preproc WHERE hasAnyTokens(s, ['hello'], 'splitByNonAlpha'));
SELECT countIf(explain LIKE '%__text_index_%') > 0 FROM (EXPLAIN actions = 1 SELECT count() FROM t_tokenized_form_preproc WHERE hasAnyTokens(s, ['hello']));

DROP TABLE t_tokenized_form_preproc;
