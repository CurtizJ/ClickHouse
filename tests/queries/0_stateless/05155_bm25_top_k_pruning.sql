-- Tags: no-parallel-replicas

-- For `ORDER BY bm25() DESC LIMIT n` the text index reader zero-fills the marks and posting-list windows whose
-- block-max score bound stays below the current top-k threshold, without decoding them. The results must not
-- change; the counters show that the pruning happened.

SET enable_analyzer = 1;
SET allow_experimental_bm25_scoring = 1;
SET query_plan_direct_read_from_text_index = 1;
SET use_skip_indexes_on_data_read = 1;
SET use_top_k_dynamic_filtering = 1;
SET query_plan_max_limit_for_top_k_optimization = 1000;
SET text_index_bm25_pruning = 1;
SET max_threads = 1;
SET log_queries = 1;

DROP TABLE IF EXISTS tab_bm25_prune;

-- Six parts of 2048 rows, four marks each. The token `common` (in 80% of the rows) has multi-segment
-- posting lists (`posting_list_block_size = 256`) whose 128-posting blocks are narrower than the marks, so a
-- mark holds several pruning windows. `rare` occurs once per part in rows of the same length: their scores tie exactly.
CREATE TABLE tab_bm25_prune
(
    id UInt32,
    body String,
    price UInt32,
    INDEX idx_body(body) TYPE text(tokenizer = 'splitByNonAlpha', posting_list_codec = 'bitpacking', posting_list_block_size = 256, enable_scoring = 1) GRANULARITY 1
)
ENGINE = MergeTree
ORDER BY id
SETTINGS index_granularity = 512, allow_experimental_text_index_scoring = 1;

SYSTEM STOP MERGES tab_bm25_prune;

INSERT INTO tab_bm25_prune SELECT number, concat('filler text', if(number % 5 != 0, ' common', ''), if(number % 2000 = 7, ' rare', ''), if(number % 100 = 7, ' mid', ''), ' ', arrayStringConcat(arrayMap(x -> 'pad', range(number % 4)), ' ')), number % 10 FROM numbers(0, 2048);
INSERT INTO tab_bm25_prune SELECT number, concat('filler text', if(number % 5 != 0, ' common', ''), if(number % 2000 = 7, ' rare', ''), if(number % 100 = 7, ' mid', ''), ' ', arrayStringConcat(arrayMap(x -> 'pad', range(number % 4)), ' ')), number % 10 FROM numbers(2048, 2048);
INSERT INTO tab_bm25_prune SELECT number, concat('filler text', if(number % 5 != 0, ' common', ''), if(number % 2000 = 7, ' rare', ''), if(number % 100 = 7, ' mid', ''), ' ', arrayStringConcat(arrayMap(x -> 'pad', range(number % 4)), ' ')), number % 10 FROM numbers(4096, 2048);
INSERT INTO tab_bm25_prune SELECT number, concat('filler text', if(number % 5 != 0, ' common', ''), if(number % 2000 = 7, ' rare', ''), if(number % 100 = 7, ' mid', ''), ' ', arrayStringConcat(arrayMap(x -> 'pad', range(number % 4)), ' ')), number % 10 FROM numbers(6144, 2048);
INSERT INTO tab_bm25_prune SELECT number, concat('filler text', if(number % 5 != 0, ' common', ''), if(number % 2000 = 7, ' rare', ''), if(number % 100 = 7, ' mid', ''), ' ', arrayStringConcat(arrayMap(x -> 'pad', range(number % 4)), ' ')), number % 10 FROM numbers(8192, 2048);
INSERT INTO tab_bm25_prune SELECT number, concat('filler text', if(number % 5 != 0, ' common', ''), if(number % 2000 = 7, ' rare', ''), if(number % 100 = 7, ' mid', ''), ' ', arrayStringConcat(arrayMap(x -> 'pad', range(number % 4)), ' ')), number % 10 FROM numbers(10240, 2048);

SELECT count() FROM tab_bm25_prune WHERE hasToken(body, 'rare');

SELECT '-- rare OR common, k = 1';
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasToken(body, 'rare') OR hasToken(body, 'common') ORDER BY bm25() DESC, id LIMIT 1
SETTINGS log_comment = 'bm25_prune_rare_or_common_k1';
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasToken(body, 'rare') OR hasToken(body, 'common') ORDER BY bm25() DESC, id LIMIT 1
SETTINGS text_index_bm25_pruning = 0;
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasToken(body, 'rare') OR hasToken(body, 'common') ORDER BY bm25() DESC, id LIMIT 1
SETTINGS use_top_k_dynamic_filtering = 0;

SELECT '-- rare OR common, k = 5 with ties exactly at the k-th score';
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasToken(body, 'rare') OR hasToken(body, 'common') ORDER BY bm25() DESC, id LIMIT 5
SETTINGS log_comment = 'bm25_prune_rare_or_common_k5';
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasToken(body, 'rare') OR hasToken(body, 'common') ORDER BY bm25() DESC, id LIMIT 5
SETTINGS text_index_bm25_pruning = 0;
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasToken(body, 'rare') OR hasToken(body, 'common') ORDER BY bm25() DESC, id LIMIT 5
SETTINGS use_top_k_dynamic_filtering = 0;

SELECT '-- rare OR common, k larger than the number of rare rows (the threshold ends up among the common rows)';
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasToken(body, 'rare') OR hasToken(body, 'common') ORDER BY bm25() DESC, id LIMIT 10;
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasToken(body, 'rare') OR hasToken(body, 'common') ORDER BY bm25() DESC, id LIMIT 10
SETTINGS text_index_bm25_pruning = 0;
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasToken(body, 'rare') OR hasToken(body, 'common') ORDER BY bm25() DESC, id LIMIT 10
SETTINGS use_top_k_dynamic_filtering = 0;

SELECT '-- hasAllTokens(rare, mid) OR common';
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasAllTokens(body, ['rare', 'mid']) OR hasToken(body, 'common') ORDER BY bm25() DESC, id LIMIT 3
SETTINGS log_comment = 'bm25_prune_all_or_common';
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasAllTokens(body, ['rare', 'mid']) OR hasToken(body, 'common') ORDER BY bm25() DESC, id LIMIT 3
SETTINGS text_index_bm25_pruning = 0;
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasAllTokens(body, ['rare', 'mid']) OR hasToken(body, 'common') ORDER BY bm25() DESC, id LIMIT 3
SETTINGS use_top_k_dynamic_filtering = 0;

SELECT '-- rare OR NOT mid: the rows kept only by the negation score 0 and never enter the top-k';
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasToken(body, 'rare') OR NOT hasToken(body, 'mid') ORDER BY bm25() DESC, id LIMIT 3
SETTINGS log_comment = 'bm25_prune_rare_or_not_mid';
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasToken(body, 'rare') OR NOT hasToken(body, 'mid') ORDER BY bm25() DESC, id LIMIT 3
SETTINGS text_index_bm25_pruning = 0;
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasToken(body, 'rare') OR NOT hasToken(body, 'mid') ORDER BY bm25() DESC, id LIMIT 3
SETTINGS use_top_k_dynamic_filtering = 0;

SELECT '-- rare OR price < 3: the rows kept only by the non-text predicate score 0';
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasToken(body, 'rare') OR price < 3 ORDER BY bm25() DESC, id LIMIT 3
SETTINGS log_comment = 'bm25_prune_rare_or_price';
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasToken(body, 'rare') OR price < 3 ORDER BY bm25() DESC, id LIMIT 3
SETTINGS text_index_bm25_pruning = 0;
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasToken(body, 'rare') OR price < 3 ORDER BY bm25() DESC, id LIMIT 3
SETTINGS use_top_k_dynamic_filtering = 0;

SELECT '-- (rare AND price > 5) OR common: a masked conjunction';
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE (hasToken(body, 'rare') AND price > 5) OR hasToken(body, 'common') ORDER BY bm25() DESC, id LIMIT 3
SETTINGS log_comment = 'bm25_prune_masked_and';
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE (hasToken(body, 'rare') AND price > 5) OR hasToken(body, 'common') ORDER BY bm25() DESC, id LIMIT 3
SETTINGS text_index_bm25_pruning = 0;
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE (hasToken(body, 'rare') AND price > 5) OR hasToken(body, 'common') ORDER BY bm25() DESC, id LIMIT 3
SETTINGS use_top_k_dynamic_filtering = 0;

SELECT '-- keys that are not the descending score: no pruning, same results';
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasToken(body, 'rare') OR hasToken(body, 'common') ORDER BY bm25() + 1 DESC, id LIMIT 3
SETTINGS log_comment = 'bm25_prune_expression_key';
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasToken(body, 'rare') OR hasToken(body, 'common') ORDER BY bm25() + 1 DESC, id LIMIT 3
SETTINGS use_top_k_dynamic_filtering = 0;
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasToken(body, 'rare') OR hasToken(body, 'common') ORDER BY bm25() ASC, id LIMIT 3
SETTINGS log_comment = 'bm25_prune_ascending_key';
SELECT id, round(bm25(), 4) FROM tab_bm25_prune WHERE hasToken(body, 'rare') OR hasToken(body, 'common') ORDER BY bm25() ASC, id LIMIT 3
SETTINGS use_top_k_dynamic_filtering = 0;

SELECT '-- pruning counters: marks are pruned whenever the key is the descending score; windows only inside marks the multi-segment token `common` shares with a surviving row';
SYSTEM FLUSH LOGS query_log;

SELECT
    log_comment,
    ProfileEvents['TextScoreMarksPruned'] > 0 AS marks_pruned,
    ProfileEvents['TextScoreWindowsPruned'] > 0 AS windows_pruned
FROM system.query_log
WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment LIKE 'bm25_prune_%'
ORDER BY log_comment;

DROP TABLE tab_bm25_prune;
