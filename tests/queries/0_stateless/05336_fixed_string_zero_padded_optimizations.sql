-- Optimizations must keep the result of a comparison with a `FixedString`, which compares zero-padded: each query is run
-- with the optimization and without it.

DROP TABLE IF EXISTS tab_string;
DROP TABLE IF EXISTS tab_fixed_string;

-- Sparse serialization of `x` makes reading in order return a wrong order regardless of the key type, so it is pinned off.
CREATE TABLE tab_string (s String, x UInt32, a Array(String)) ENGINE = MergeTree ORDER BY (s, x) SETTINGS index_granularity = 1, ratio_of_defaults_for_sparse_serialization = 1;
INSERT INTO tab_string SELECT ['ab', 'ab\0', 'ab\0\0', 'zz'][number % 4 + 1] AS s, number, [s] FROM numbers(8);

CREATE TABLE tab_fixed_string (f FixedString(3), x UInt32) ENGINE = MergeTree ORDER BY (f, x) SETTINGS index_granularity = 1, ratio_of_defaults_for_sparse_serialization = 1;
INSERT INTO tab_fixed_string SELECT ['ab', 'zz'][number % 2 + 1], number FROM numbers(4);

SELECT '-- redundant comparisons';
SELECT groupArray(x) FROM (SELECT x FROM tab_string WHERE s = toFixedString('ab', 4) AND s = 'ab\0' ORDER BY x) SETTINGS optimize_redundant_comparisons = 1;
SELECT groupArray(x) FROM (SELECT x FROM tab_string WHERE s = toFixedString('ab', 4) AND s = 'ab\0' ORDER BY x) SETTINGS optimize_redundant_comparisons = 0;
SELECT groupArray(x) FROM (SELECT x FROM tab_fixed_string WHERE f = 'ab' AND f = 'ab\0' ORDER BY x) SETTINGS optimize_redundant_comparisons = 1;
SELECT groupArray(x) FROM (SELECT x FROM tab_fixed_string WHERE f = 'ab' AND f = 'ab\0' ORDER BY x) SETTINGS optimize_redundant_comparisons = 0;
SELECT groupArray(x) FROM (SELECT x FROM tab_fixed_string WHERE f = 'ab' AND f < 'ab\0\0\0' ORDER BY x) SETTINGS optimize_redundant_comparisons = 1;
SELECT groupArray(x) FROM (SELECT x FROM tab_fixed_string WHERE f = 'ab' AND f < 'ab\0\0\0' ORDER BY x) SETTINGS optimize_redundant_comparisons = 0;
SELECT groupArray(x) FROM (SELECT x FROM tab_string WHERE s != toFixedString('ab', 4) AND s != 'zz' ORDER BY x) SETTINGS optimize_redundant_comparisons = 1;
SELECT groupArray(x) FROM (SELECT x FROM tab_string WHERE s != toFixedString('ab', 4) AND s != 'zz' ORDER BY x) SETTINGS optimize_redundant_comparisons = 0;

SELECT '-- chain of equalities to IN';
SELECT groupArray(x) FROM (SELECT x FROM tab_string WHERE s = toFixedString('ab', 4) OR s = 'zz' ORDER BY x) SETTINGS optimize_min_equality_disjunction_chain_length = 1;
SELECT groupArray(x) FROM (SELECT x FROM tab_string WHERE s = toFixedString('ab', 4) OR s = 'zz' ORDER BY x) SETTINGS optimize_min_equality_disjunction_chain_length = 100;
SELECT groupArray(x) FROM (SELECT x FROM tab_fixed_string WHERE f = 'ab\0' OR f = toFixedString('zz', 5) ORDER BY x) SETTINGS optimize_min_equality_disjunction_chain_length = 1;
SELECT groupArray(x) FROM (SELECT x FROM tab_fixed_string WHERE f = 'ab\0' OR f = toFixedString('zz', 5) ORDER BY x) SETTINGS optimize_min_equality_disjunction_chain_length = 100;
SELECT groupArray(x) FROM (SELECT x FROM tab_string WHERE s != toFixedString('ab', 4) AND s != 'yy' ORDER BY x) SETTINGS optimize_min_inequality_conjunction_chain_length = 1;
SELECT groupArray(x) FROM (SELECT x FROM tab_string WHERE s != toFixedString('ab', 4) AND s != 'yy' ORDER BY x) SETTINGS optimize_min_inequality_conjunction_chain_length = 100;

SELECT '-- arrayExists to has';
SELECT groupArray(x) FROM (SELECT x FROM tab_string WHERE arrayExists(e -> e = toFixedString('ab', 4), a) ORDER BY x) SETTINGS optimize_rewrite_array_exists_to_has = 1;
SELECT groupArray(x) FROM (SELECT x FROM tab_string WHERE arrayExists(e -> e = toFixedString('ab', 4), a) ORDER BY x) SETTINGS optimize_rewrite_array_exists_to_has = 0;

SELECT '-- toFixedString is not injective for a String';
SELECT count() FROM (SELECT toFixedString(s, 4) AS k FROM tab_string GROUP BY k) SETTINGS optimize_injective_functions_in_group_by = 1;
SELECT count() FROM (SELECT toFixedString(s, 4) AS k FROM tab_string GROUP BY k) SETTINGS optimize_injective_functions_in_group_by = 0;
SELECT uniqExact(toFixedString(s, 4)) FROM tab_string SETTINGS optimize_injective_functions_inside_uniq = 1;
SELECT uniqExact(toFixedString(s, 4)) FROM tab_string SETTINGS optimize_injective_functions_inside_uniq = 0;
SELECT count() FROM (SELECT x FROM tab_string ORDER BY x LIMIT 1 BY toFixedString(s, 4)) SETTINGS optimize_injective_functions_in_limit_by = 1;
SELECT count() FROM (SELECT x FROM tab_string ORDER BY x LIMIT 1 BY toFixedString(s, 4)) SETTINGS optimize_injective_functions_in_limit_by = 0;

SELECT '-- read in order: a String key compared with a FixedString constant is not fixed';
SELECT groupArray(x) FROM (SELECT x FROM tab_string WHERE s = toFixedString('ab', 4) ORDER BY x) SETTINGS optimize_read_in_order = 1, max_threads = 1;
SELECT groupArray(x) FROM (SELECT x FROM tab_string WHERE s = toFixedString('ab', 4) ORDER BY x) SETTINGS optimize_read_in_order = 0;
SELECT groupArray(x) FROM (SELECT x FROM tab_fixed_string WHERE f = 'ab\0' ORDER BY x) SETTINGS optimize_read_in_order = 1, max_threads = 1;
SELECT groupArray(x) FROM (SELECT x FROM tab_fixed_string WHERE f = 'ab\0' ORDER BY x) SETTINGS optimize_read_in_order = 0;

DROP TABLE tab_string;
DROP TABLE tab_fixed_string;
