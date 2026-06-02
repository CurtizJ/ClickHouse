-- Tags: no-parallel-replicas

-- Tests direct read from the text index as a hint for the OR-of-queries functions match,
-- multiSearchAny, multiSearchAnyUTF8 and multiMatchAny. Each of these may expand into several
-- token queries that are OR-ed together, so the optimizer reads one virtual column per branch and
-- combines them with OR before AND-ing the original predicate.

SET enable_analyzer = 1;
SET use_skip_indexes_on_data_read = 1;
SET query_plan_direct_read_from_text_index = 1; -- CI may inject False; without it the optimization is skipped
SET query_plan_text_index_add_hint = 1;         -- enables Hint mode
SET text_index_hint_max_selectivity = 1.0;      -- always keep the hint so TextIndexUseHint fires deterministically

DROP TABLE IF EXISTS tab;

-- ngrams(3): a needle's n-grams are required tokens, so a substring / regex search can use the index.
CREATE TABLE tab
(
    id UInt32,
    s String,
    INDEX idx(s) TYPE text(tokenizer = ngrams(3)) GRANULARITY 1
)
ENGINE = MergeTree
ORDER BY id
SETTINGS index_granularity = 4;

-- 'foobar' and 'helloworld' occur in disjoint rows, so an OR over the two needles must return both
-- groups (160 + 160 = 320). An accidental AND of the branches would return 0 rows.
INSERT INTO tab SELECT
    number,
    multiIf(number % 50 = 0, 'foobar', number % 50 = 1, 'helloworld', concat('filler', toString(number)))
FROM numbers(8000);

SELECT '-- results are identical with and without direct read from the text index';

SELECT 'multiSearchAny',
    (SELECT count() FROM tab WHERE multiSearchAny(s, ['foobar', 'helloworld']) SETTINGS query_plan_direct_read_from_text_index = 0),
    (SELECT count() FROM tab WHERE multiSearchAny(s, ['foobar', 'helloworld']));

SELECT 'multiSearchAnyUTF8',
    (SELECT count() FROM tab WHERE multiSearchAnyUTF8(s, ['foobar', 'helloworld']) SETTINGS query_plan_direct_read_from_text_index = 0),
    (SELECT count() FROM tab WHERE multiSearchAnyUTF8(s, ['foobar', 'helloworld']));

SELECT 'match',
    (SELECT count() FROM tab WHERE match(s, 'foobar|helloworld') SETTINGS query_plan_direct_read_from_text_index = 0),
    (SELECT count() FROM tab WHERE match(s, 'foobar|helloworld'));

SELECT 'multiMatchAny',
    (SELECT count() FROM tab WHERE multiMatchAny(s, ['foobar', 'helloworld']) SETTINGS query_plan_direct_read_from_text_index = 0),
    (SELECT count() FROM tab WHERE multiMatchAny(s, ['foobar', 'helloworld']));

SELECT '-- the hint engages for each function (one tagged direct-read query each)';

SELECT 'dread_multiSearchAny',     count() FROM tab WHERE multiSearchAny(s, ['foobar', 'helloworld']);
SELECT 'dread_multiSearchAnyUTF8', count() FROM tab WHERE multiSearchAnyUTF8(s, ['foobar', 'helloworld']);
SELECT 'dread_match',              count() FROM tab WHERE match(s, 'foobar|helloworld');
SELECT 'dread_multiMatchAny',      count() FROM tab WHERE multiMatchAny(s, ['foobar', 'helloworld']);

SYSTEM FLUSH LOGS query_log;

-- For each tagged direct-read query: the hint was used (TextIndexUseHint > 0) and never discarded.
SELECT
    extract(query, 'dread_[A-Za-z0-9]+') AS q,
    sum(ProfileEvents['TextIndexUseHint']) > 0,
    sum(ProfileEvents['TextIndexDiscardHint'])
FROM system.query_log
WHERE event_date >= yesterday() AND event_time >= now() - 600
  AND current_database = currentDatabase() AND type = 'QueryFinish'
  AND query LIKE 'SELECT ''dread_%'
GROUP BY q
ORDER BY q;

DROP TABLE tab;
