-- The text-search functions take the preprocessor and the postprocessor of a text index as lambdas after the tokenizer.
-- The query plan passes those of the index, so the functions match the rows the index finds without reading it.

SELECT '-- The preprocessor applies to the input and a String needle';
SELECT hasAnyTokens('Hello World', 'hello', 'splitByNonAlpha', 'x -> lower(x)');
SELECT hasAnyTokens('Hello World', 'hello', 'splitByNonAlpha');
SELECT hasAllTokens('Hello World', 'HELLO world', 'splitByNonAlpha', 'x -> lower(x)');
SELECT hasAllTokens('Hello World', ['HELLO'], 'splitByNonAlpha', 'x -> lower(x)'); -- Array needles are tokens as they are
SELECT hasAnyTokens(['Foo Bar', 'Baz'], 'BAR', 'splitByNonAlpha', 'value -> lower(value)'); -- to every element of an Array
SELECT hasPhrase('The Quick Fox', 'quick FOX', 'splitByNonAlpha', 'x -> lower(x)');

SELECT '-- The postprocessor applies to every token, the dropped ones do not separate a phrase';
SELECT hasAllTokens('Hello World', ['hello'], 'splitByNonAlpha', '', 'x -> lower(x)');
SELECT hasPhrase('see the cat', 'see cat', 'splitByNonAlpha', '', 'x -> if(x IN (''the''), '''', x)');
SELECT hasPhrase('see a cat', 'see cat', 'splitByNonAlpha', '', 'x -> if(x IN (''the''), '''', x)');
SELECT hasAnyTokens('the', 'the', 'splitByNonAlpha', '', 'x -> if(x IN (''the''), '''', x)');
SELECT hasAnyTokens(materialize('ab cd'), 'abc', 'ngrams(3)', 'x -> replaceAll(x, '' '', '''')', 'x -> upper(x)');

SELECT '-- NULL';
SELECT hasAnyTokens(materialize(CAST(NULL AS Nullable(String))), 'x', 'splitByNonAlpha', '', 'x -> lower(x)');
SELECT hasAnyTokens(materialize(CAST(NULL AS Nullable(String))), 'default', 'splitByNonAlpha', 'x -> ifNull(x, ''default'')');
SELECT hasAnyTokens(materialize(toNullable('Hello')), 'hello', 'splitByNonAlpha', 'x -> lower(x)');
SELECT hasPhrase(materialize(CAST(NULL AS Nullable(String))), 'a b', 'splitByNonAlpha', 'x -> lower(x)');

SELECT '-- Invalid transforms';
SELECT hasAnyTokens('a', 'a', 'splitByNonAlpha', 'lower(x)'); -- { serverError BAD_ARGUMENTS }
SELECT hasAnyTokens('a', 'a', 'splitByNonAlpha', '(x, y) -> lower(x)'); -- { serverError BAD_ARGUMENTS }
SELECT hasAnyTokens('a', 'a', 'splitByNonAlpha', 'x -> if(x IN (SELECT ''a''), x, '''')'); -- { serverError BAD_ARGUMENTS }
SELECT hasAnyTokens('a', 'a', 'splitByNonAlpha', 'x -> concat(x, toString(rand()))'); -- { serverError INCORRECT_QUERY }
SELECT hasAnyTokens('a', 'a', 'splitByNonAlpha', 'x -> arrayJoin([x])'); -- { serverError INCORRECT_QUERY }
SELECT hasAnyTokens('a', 'a', 'splitByNonAlpha', 'x -> x'); -- { serverError INCORRECT_QUERY }
SELECT hasAnyTokens('a', 'a', 'splitByNonAlpha', 'x -> length(x)'); -- { serverError INCORRECT_QUERY }
SELECT hasAnyTokens('a', 'a', 'splitByNonAlpha', '', 'x -> [x]'); -- { serverError INCORRECT_QUERY }
SELECT hasAnyTokens('a', 'a', 'splitByNonAlpha', 'x -> lower(y)'); -- { serverError UNKNOWN_IDENTIFIER }
SELECT hasPhrase('a', 'a', 'splitByNonAlpha', materialize('x -> lower(x)')); -- { serverError ILLEGAL_COLUMN }

DROP TABLE IF EXISTS tab;

CREATE TABLE tab
(
    id UInt32,
    s Nullable(String)
)
ENGINE = MergeTree ORDER BY id SETTINGS index_granularity = 1;

-- The first part has no index, its direct read evaluates the predicate.
INSERT INTO tab VALUES (1, 'See the Cat'), (2, 'see a cat'), (3, NULL), (4, 'Dog');
ALTER TABLE tab ADD INDEX idx s TYPE text(tokenizer = splitByNonAlpha, preprocessor = lower(s), postprocessor = if(s IN ('the'), '', s));
INSERT INTO tab VALUES (5, 'SEE THE CAT'), (6, 'hot dog'), (7, NULL);

SELECT '-- Same rows with and without the direct read';
SELECT groupArray(id) FROM (SELECT id FROM tab WHERE hasPhrase(s, 'SEE CAT') ORDER BY id);
SELECT groupArray(id) FROM (SELECT id FROM tab WHERE hasPhrase(s, 'SEE CAT') ORDER BY id) SETTINGS query_plan_direct_read_from_text_index = 0;
SELECT groupArray(id) FROM (SELECT id FROM tab WHERE hasToken(s, 'DOG') ORDER BY id);
SELECT groupArray(id) FROM (SELECT id FROM tab WHERE hasToken(s, 'DOG') ORDER BY id) SETTINGS query_plan_direct_read_from_text_index = 0;
SELECT groupArray(id) FROM (SELECT id FROM tab WHERE hasAllTokens(s, 'Cat the') ORDER BY id);
SELECT groupArray(id) FROM (SELECT id FROM tab WHERE hasAllTokens(s, 'Cat the') ORDER BY id) SETTINGS query_plan_direct_read_from_text_index = 0;

SELECT '-- NOT keeps the NULL values out with a postprocessor';
SELECT groupArray(id) FROM (SELECT id FROM tab WHERE NOT hasAnyTokens(s, 'hot') ORDER BY id) SETTINGS use_skip_indexes = 0;
SELECT groupArray(id) FROM (SELECT id FROM tab WHERE NOT hasAnyTokens(s, 'hot') ORDER BY id) SETTINGS query_plan_direct_read_from_text_index = 0;

SELECT '-- The plan passes the transforms of the index';
SELECT count() > 0 FROM (EXPLAIN actions = 1 SELECT id FROM tab WHERE hasAnyTokens(s, 'cat') SETTINGS query_plan_direct_read_from_text_index = 0)
WHERE explain LIKE '%x -> lower(x)%' AND explain LIKE '%x -> if(%';

SELECT '-- The index answers the arguments that denote its transforms';
SELECT groupArray(id) FROM (SELECT id FROM tab WHERE hasAnyTokens(s, 'CAT', 'splitByNonAlpha', 'y -> lower(y)', 'x -> if(x IN (''the''), '''', x)') ORDER BY id)
SETTINGS force_data_skipping_indices = 'idx';
SELECT groupArray(id) FROM (SELECT id FROM tab WHERE hasAnyTokens(s, 'CAT', 'splitByNonAlpha', 'x -> upper(x)', 'x -> if(x IN (''the''), '''', x)') ORDER BY id)
SETTINGS force_data_skipping_indices = 'idx'; -- { serverError INDEX_NOT_USED }
SELECT groupArray(id) FROM (SELECT id FROM tab WHERE hasAnyTokens(s, 'CAT', 'splitByNonAlpha', 'x -> lower(x)') ORDER BY id)
SETTINGS force_data_skipping_indices = 'idx'; -- { serverError INDEX_NOT_USED }

DROP TABLE tab;
