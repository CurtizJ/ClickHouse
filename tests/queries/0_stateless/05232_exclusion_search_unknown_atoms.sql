-- The generic exclusion search over the primary key splits a mark range until the condition is either
-- impossible or certain on it. A conjunct that the key analysis cannot evaluate (here a condition on the
-- non-key column `c` or `s`) is never certain, so it used to make the search split every range that may
-- match down to single marks although it cannot exclude any of them. Such an atom is now assumed true
-- during the search: the selected marks are the same as for the key part of the condition alone and the
-- step budget that is enough for the key part alone is enough for the whole condition.

DROP TABLE IF EXISTS t_exclusion_search_unknown_atoms;

CREATE TABLE t_exclusion_search_unknown_atoms (a UInt8, b UInt32, c UInt32, s String)
ENGINE = MergeTree ORDER BY (a, b)
SETTINGS index_granularity = 8;

-- Three values of the first key column, so that a condition on `b` alone cannot use the binary search
-- and the exclusion search finds one run of marks per value of `a`.
INSERT INTO t_exclusion_search_unknown_atoms
SELECT number % 3, intDiv(number, 3), number, toString(number) FROM numbers(30000);

OPTIMIZE TABLE t_exclusion_search_unknown_atoms FINAL;

-- The condition on `b` alone needs about 200 steps with this table; with a non-key conjunct, the
-- search used to need about 2300 steps, one per selected mark.
-- Every query runs with the budget and without it. The results have to be the same, and the budget must
-- not be reached, so that the selected marks are the same as well. The `sum` keeps the exact-count
-- optimization from replacing the read of the selected marks.

SET merge_tree_coarse_index_granularity = 8;
-- The query condition cache would pre-split the ranges for repetitions of the same predicate.
SET use_query_condition_cache = 0;

-- { echoOn }

SELECT count(), sum(c) FROM t_exclusion_search_unknown_atoms
WHERE b BETWEEN 2000 AND 7000
SETTINGS merge_tree_generic_exclusion_search_max_steps = 500, log_comment = '05232_unknown_atoms range budget';
SELECT count(), sum(c) FROM t_exclusion_search_unknown_atoms
WHERE b BETWEEN 2000 AND 7000
SETTINGS merge_tree_generic_exclusion_search_max_steps = 0, log_comment = '05232_unknown_atoms range unlimited';

SELECT count(), sum(c) FROM t_exclusion_search_unknown_atoms
WHERE b BETWEEN 2000 AND 7000 AND c % 7 = 1
SETTINGS merge_tree_generic_exclusion_search_max_steps = 500, log_comment = '05232_unknown_atoms and_unknown budget';
SELECT count(), sum(c) FROM t_exclusion_search_unknown_atoms
WHERE b BETWEEN 2000 AND 7000 AND c % 7 = 1
SETTINGS merge_tree_generic_exclusion_search_max_steps = 0, log_comment = '05232_unknown_atoms and_unknown unlimited';

SELECT count(), sum(c) FROM t_exclusion_search_unknown_atoms
WHERE b BETWEEN 2000 AND 7000 AND NOT startsWith(s, '1')
SETTINGS merge_tree_generic_exclusion_search_max_steps = 500, log_comment = '05232_unknown_atoms and_not_unknown budget';
SELECT count(), sum(c) FROM t_exclusion_search_unknown_atoms
WHERE b BETWEEN 2000 AND 7000 AND NOT startsWith(s, '1')
SETTINGS merge_tree_generic_exclusion_search_max_steps = 0, log_comment = '05232_unknown_atoms and_not_unknown unlimited';

SELECT count(), sum(c) FROM t_exclusion_search_unknown_atoms
WHERE (b < 3000 OR c % 7 = 1) AND b BETWEEN 2000 AND 7000
SETTINGS merge_tree_generic_exclusion_search_max_steps = 500, log_comment = '05232_unknown_atoms or_unknown budget';
SELECT count(), sum(c) FROM t_exclusion_search_unknown_atoms
WHERE (b < 3000 OR c % 7 = 1) AND b BETWEEN 2000 AND 7000
SETTINGS merge_tree_generic_exclusion_search_max_steps = 0, log_comment = '05232_unknown_atoms or_unknown unlimited';

SYSTEM FLUSH LOGS query_log;

WITH (
    SELECT max(ProfileEvents['SelectedMarks']) FROM system.query_log
    WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment = '05232_unknown_atoms range unlimited'
) AS marks_of_range_alone
SELECT
    splitByChar(' ', log_comment)[2] AS test_case,
    maxIf(ProfileEvents['IndexGenericExclusionSearchStepLimitReached'], splitByChar(' ', log_comment)[3] = 'budget') AS step_limit_reached_with_budget,
    maxIf(ProfileEvents['SelectedMarks'], splitByChar(' ', log_comment)[3] = 'budget')
        = maxIf(ProfileEvents['SelectedMarks'], splitByChar(' ', log_comment)[3] = 'unlimited') AS same_marks_as_unlimited,
    maxIf(ProfileEvents['SelectedMarks'], splitByChar(' ', log_comment)[3] = 'unlimited') = marks_of_range_alone AS same_marks_as_range_alone
FROM system.query_log
WHERE current_database = currentDatabase() AND type = 'QueryFinish' AND log_comment LIKE '05232_unknown_atoms %'
GROUP BY test_case
ORDER BY test_case;

-- The exact-count optimization relies on ranges where every row matches the whole condition. The
-- assumption about unknown atoms must not leak into them: the count must not change with the budget or
-- with the optimization.
SELECT count() FROM t_exclusion_search_unknown_atoms
WHERE b BETWEEN 2000 AND 7000 AND c % 7 = 1
SETTINGS optimize_use_projections = 1, optimize_use_implicit_projections = 1, merge_tree_generic_exclusion_search_max_steps = 500;
SELECT count() FROM t_exclusion_search_unknown_atoms
WHERE b BETWEEN 2000 AND 7000 AND c % 7 = 1
SETTINGS optimize_use_projections = 0, optimize_use_implicit_projections = 0, merge_tree_generic_exclusion_search_max_steps = 500;
SELECT count() FROM t_exclusion_search_unknown_atoms
WHERE b BETWEEN 2000 AND 7000
SETTINGS optimize_use_projections = 1, optimize_use_implicit_projections = 1, merge_tree_generic_exclusion_search_max_steps = 500;
SELECT count() FROM t_exclusion_search_unknown_atoms
WHERE b BETWEEN 2000 AND 7000
SETTINGS optimize_use_projections = 0, optimize_use_implicit_projections = 0, merge_tree_generic_exclusion_search_max_steps = 500;

-- { echoOff }

DROP TABLE t_exclusion_search_unknown_atoms;
