-- The intersection scorer of `_bm25_score` (global `All` search mode, e.g. `hasAllTokens`) chooses between the
-- joint leapfrog over the posting lists and a linear counting pass, by the same rule as the lazy `AND`:
-- the leapfrog pays off only when the sparsest list can skip whole packed blocks (128 postings) of the
-- densest one, i.e. when `min_density * 128 < max_density`. This test pins the rule for the default and the
-- forcing values of `text_index_lazy_intersection_density_threshold` (0 -> linear pass, 1 -> leapfrog) and
-- checks that both paths produce bit-identical scores.

SET enable_analyzer = 1;
SET allow_experimental_bm25_score_column = 1;
SET query_plan_direct_read_from_text_index = 1;
SET use_skip_indexes_on_data_read = 1;
SET text_index_posting_list_apply_mode = 'lazy';
SET use_query_condition_cache = 0;
SET merge_tree_read_split_ranges_into_intersecting_and_non_intersecting_injection_probability = 0;
SET log_queries = 1;
SET log_profile_events = 1;

DROP TABLE IF EXISTS tab_scorer_rule;
DROP TABLE IF EXISTS scores_linear;
DROP TABLE IF EXISTS scores_leapfrog;

-- posting_list_block_size = 256: tokens with more than 256 postings take the multi-segment cursor path,
-- `dsparse` (2 postings) is embedded. index_granularity = 1024 splits the part into 8 scoring windows, so
-- packed blocks of the sparser lists straddle windows.
--   adense  : every row 0..7999                     -> density 1.0
--   bmid    : number % 10 = 0, twice on every 80th  -> 800 docs, density 0.1, term frequency 1 or 2
--   cmid    : number % 8 = 0                        -> 1000 docs, density 0.125
--   dsparse : number % 4000 = 0                     -> 2 docs, density 0.00025
-- The fillers vary the document lengths of the intersection rows (multiples of 40).
CREATE TABLE tab_scorer_rule
(
    k UInt64,
    s String,
    INDEX idx s TYPE text(
        tokenizer = 'splitByNonAlpha',
        posting_list_codec = 'bitpacking',
        posting_list_block_size = 256,
        scoring = 'bm25')
)
ENGINE = MergeTree ORDER BY k
SETTINGS index_granularity = 1024, index_granularity_bytes = '10M', allow_experimental_text_index_scoring = 1;

INSERT INTO tab_scorer_rule
SELECT number,
    concat(
        'adense',
        multiIf(number % 80 = 0, ' bmid bmid', number % 10 = 0, ' bmid', ''),
        if(number % 8 = 0, ' cmid', ''),
        if(number % 4000 = 0, ' dsparse', ''),
        arrayStringConcat(arrayMap(i -> concat(' f', toString(i)), range(intDiv(number, 40) % 4))))
FROM numbers(8000);

CREATE TABLE scores_linear (tag String, k UInt64, score Float32) ENGINE = Memory;
CREATE TABLE scores_leapfrog (tag String, k UInt64, score Float32) ENGINE = Memory;

-- Two mid-density lists (0.1 and 0.125): both below the 0.2 threshold, but 0.1 * 128 >= 0.125,
-- so the sparsest list has a posting in every block of the densest one -> linear pass.
INSERT INTO scores_linear SELECT 'mid_pair', k, _bm25_score FROM tab_scorer_rule WHERE hasAllTokens(s, ['bmid', 'cmid'])
    SETTINGS log_comment = '05219_rule_mid_pair_default';

-- Threshold 1 forces the leapfrog even where the rule would pick the linear pass.
INSERT INTO scores_leapfrog SELECT 'mid_pair', k, _bm25_score FROM tab_scorer_rule WHERE hasAllTokens(s, ['bmid', 'cmid'])
    SETTINGS text_index_lazy_intersection_density_threshold = 1.0, log_comment = '05219_rule_mid_pair_threshold_1';

-- Sparse against dense (0.00025 * 128 < 1.0): the leapfrog can skip almost every block.
INSERT INTO scores_leapfrog SELECT 'sparse_dense', k, _bm25_score FROM tab_scorer_rule WHERE hasAllTokens(s, ['adense', 'dsparse'])
    SETTINGS log_comment = '05219_rule_sparse_dense_default';

-- Threshold 0 forces the linear pass even where the rule would pick the leapfrog (embedded and compressed cursors).
INSERT INTO scores_linear SELECT 'sparse_dense', k, _bm25_score FROM tab_scorer_rule WHERE hasAllTokens(s, ['adense', 'dsparse'])
    SETTINGS text_index_lazy_intersection_density_threshold = 0.0, log_comment = '05219_rule_sparse_dense_threshold_0';

-- Three lists (1.0, 0.1, 0.125): 0.1 * 128 >= 1.0 -> linear pass by default, leapfrog with threshold 1.
INSERT INTO scores_linear SELECT 'triple', k, _bm25_score FROM tab_scorer_rule WHERE hasAllTokens(s, ['adense', 'bmid', 'cmid'])
    SETTINGS log_comment = '05219_rule_triple_default';

INSERT INTO scores_leapfrog SELECT 'triple', k, _bm25_score FROM tab_scorer_rule WHERE hasAllTokens(s, ['adense', 'bmid', 'cmid'])
    SETTINGS text_index_lazy_intersection_density_threshold = 1.0, log_comment = '05219_rule_triple_threshold_1';

SELECT '-- both paths score the same rows with bit-identical scores';
SELECT
    l.tag,
    count() AS rows,
    countIf(l.score != r.score) AS mismatches,
    round(min(l.score), 4) AS min_score,
    round(max(l.score), 4) AS max_score
FROM scores_linear AS l
FULL OUTER JOIN scores_leapfrog AS r ON l.tag = r.tag AND l.k = r.k
GROUP BY l.tag
ORDER BY l.tag;

SELECT '-- the linear pass scores exactly the intersection rows';
SELECT tag, count() = (SELECT count() FROM tab_scorer_rule WHERE hasAllTokens(s, ['bmid', 'cmid'])) FROM scores_linear WHERE tag = 'mid_pair' GROUP BY tag;
SELECT tag, count() = (SELECT count() FROM tab_scorer_rule WHERE hasAllTokens(s, ['adense', 'dsparse'])) FROM scores_linear WHERE tag = 'sparse_dense' GROUP BY tag;
SELECT tag, count() = (SELECT count() FROM tab_scorer_rule WHERE hasAllTokens(s, ['adense', 'bmid', 'cmid'])) FROM scores_linear WHERE tag = 'triple' GROUP BY tag;

SELECT '-- scores of the linear pass: term frequency 2 on the multiples of 80, document length grows with k % 160';
SELECT k, round(score, 4) FROM scores_linear WHERE tag = 'mid_pair' AND k IN (0, 40, 80, 120, 160, 200, 240, 280) ORDER BY k;

SYSTEM FLUSH LOGS query_log;

SELECT '-- the algorithm chosen per query';
-- Under parallel replicas the counters land on the replica rows; resolve the initiator rows by
-- `current_database` and aggregate every row of the same `initial_query_id`.
WITH initial_queries AS
(
    SELECT query_id, log_comment
    FROM system.query_log
    WHERE event_date >= yesterday() AND event_time >= now() - 600
      AND current_database = currentDatabase()
      AND type = 'QueryFinish'
      AND is_initial_query = 1
      AND log_comment LIKE '05219_rule_%'
)
SELECT
    iq.log_comment AS tag,
    sum(ql.ProfileEvents['TextScoreLinearIntersections']) > 0 AS linear_pass,
    sum(ql.ProfileEvents['TextScoreLeapfrogIntersections']) > 0 AS leapfrog
FROM system.query_log AS ql
INNER JOIN initial_queries AS iq ON ql.initial_query_id = iq.query_id
WHERE ql.event_date >= yesterday() AND ql.event_time >= now() - 600
  AND ql.type = 'QueryFinish'
GROUP BY tag
ORDER BY tag;

DROP TABLE tab_scorer_rule;
DROP TABLE scores_linear;
DROP TABLE scores_leapfrog;
