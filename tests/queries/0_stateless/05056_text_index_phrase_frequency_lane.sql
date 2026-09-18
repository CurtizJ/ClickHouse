-- The text index positions stream stores one dense bit-packed `freq - 1` per document per doc-block.
-- Exercise the frequency lane across its interesting shapes: blocks where every document holds the
-- token once (the lane collapses to a PFor constant block), blocks with a lone high-frequency
-- document among single-occurrence ones (the frequency becomes a patched PFor exception), and
-- documents whose frequency alone fills more than one position-lane block.

-- Every token below occurs in nearly every document, which is exactly the shape the phrase
-- selectivity hint declines to serve from the index. Raise the threshold so `hasPhrase` really goes
-- through the positions stream instead of re-evaluating on the physical column.
SET text_index_hint_max_selectivity = 1;

DROP TABLE IF EXISTS tab;

CREATE TABLE tab
(
    id UInt32,
    message String,
    INDEX idx(message) TYPE text(tokenizer = splitByNonAlpha, support_phrase_search = 1)
)
ENGINE = MergeTree
ORDER BY id
SETTINGS allow_experimental_text_index_phrase_search = 1, index_granularity = 8192;

-- 500 documents span several doc-blocks of 128 postings. Every document contains 'alpha beta' once,
-- so the frequency lane of a full block is all ones. Every 137th document repeats 'alpha beta' many
-- times, which is the lone spike the lane has to patch rather than widen the base for.
INSERT INTO tab
SELECT
    number,
    arrayStringConcat(arrayMap(x -> 'alpha beta gamma', range(if(number % 137 = 0, 400, 1))), ' ')
FROM numbers(500);

-- A single document whose term frequency exceeds one position-lane PFor block on its own.
INSERT INTO tab
SELECT 100000, arrayStringConcat(arrayMap(x -> 'delta epsilon', range(1000)), ' ');

SELECT count() FROM tab;

SELECT 'phrase present in every document';
SELECT count() FROM tab WHERE hasPhrase(message, 'alpha beta');
SELECT count() FROM tab WHERE hasPhrase(message, 'beta gamma');
SELECT count() FROM tab WHERE hasPhrase(message, 'alpha beta gamma');

SELECT 'phrase spanning a repetition boundary: only the repeating documents match';
SELECT count() FROM tab WHERE hasPhrase(message, 'gamma alpha');

SELECT 'phrase absent';
SELECT count() FROM tab WHERE hasPhrase(message, 'beta alpha');
SELECT count() FROM tab WHERE hasPhrase(message, 'alpha gamma');

SELECT 'high-frequency document';
SELECT count() FROM tab WHERE hasPhrase(message, 'delta epsilon');
SELECT count() FROM tab WHERE hasPhrase(message, 'epsilon delta');

SELECT 'the same after a merge re-encodes the positions';
OPTIMIZE TABLE tab FINAL;
SELECT count() FROM system.parts WHERE database = currentDatabase() AND table = 'tab' AND active;
SELECT count() FROM tab WHERE hasPhrase(message, 'alpha beta gamma');
SELECT count() FROM tab WHERE hasPhrase(message, 'gamma alpha');
SELECT count() FROM tab WHERE hasPhrase(message, 'delta epsilon');
SELECT count() FROM tab WHERE hasPhrase(message, 'epsilon delta');

SELECT 'the index-based result agrees with a full scan';
SELECT
    (SELECT count() FROM tab WHERE hasPhrase(message, 'alpha beta gamma')) = (SELECT count() FROM tab WHERE position(message, 'alpha beta gamma') > 0),
    (SELECT count() FROM tab WHERE hasPhrase(message, 'gamma alpha')) = (SELECT count() FROM tab WHERE position(message, 'gamma alpha') > 0),
    (SELECT count() FROM tab WHERE hasPhrase(message, 'delta epsilon')) = (SELECT count() FROM tab WHERE position(message, 'delta epsilon') > 0),
    (SELECT count() FROM tab WHERE hasPhrase(message, 'beta alpha')) = (SELECT count() FROM tab WHERE position(message, 'beta alpha') > 0);

SELECT 'the positions stream really was read';

-- A phrase this test has not asked for yet, so the per-(part, query) phrase cache cannot answer it
-- and the positions blocks are decoded for real.
SELECT count() FROM tab WHERE hasPhrase(message, 'beta gamma alpha') SETTINGS log_comment = '05056_frequency_lane';

SYSTEM FLUSH LOGS query_log;

SELECT ProfileEvents['TextIndexPositionsBlocksRead'] > 0
FROM system.query_log
WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment = '05056_frequency_lane'
ORDER BY event_time_microseconds DESC
LIMIT 1;

DROP TABLE tab;
