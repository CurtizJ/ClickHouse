-- Tests JIT compilation of regular expressions with alternation, which must give the same results as RE2:
-- branches are tried in order (leftmost-first), and captures of a failed branch are reset.
-- The reference is produced by RE2 (`compile_regular_expressions = 0`).

SET compile_regular_expressions = 1;
SET min_count_to_compile_regular_expression = 0;

DROP TABLE IF EXISTS strings;
CREATE TABLE strings (s String) ENGINE = Memory;
INSERT INTO strings VALUES
    (''), ('a'), ('ab'), ('abc'), ('abcd'), ('xabcd'), ('xab'), ('b'), ('cd'), ('aab'), ('abab'),
    ('user=alice action=login'), ('user=bob ip=10.0.0.1'), ('foo=12 bar=345 baz=6'), ('[INFO] a = b ] c'),
    ('k: v, k2 : v2; k3=v3'), ('USER=Alice'), ('e ea eb ef eg'), ('12 abc 3456 de');

SELECT '-- order of branches';
SELECT 'a|ab', groupArray(extractAll(s, 'a|ab')) FROM strings;
SELECT 'ab|a', groupArray(extractAll(s, 'ab|a')) FROM strings;
SELECT 'x(a|ab)(c|bcd)', groupArray(extractAll(s, 'x(a|ab)(c|bcd)')) FROM strings;
SELECT '(ab|a)(bc|c)?', groupArray(extractAll(s, '(ab|a)(bc|c)?')) FROM strings;

SELECT '-- captures of a failed branch';
SELECT '(a)|(b)', groupArray(extractAll(s, '(a)|(b)')) FROM strings;
SELECT '([a-z]+)=|([0-9]+)', groupArray(extractAll(s, '([a-z]+)=|([0-9]+)')) FROM strings;
SELECT '(foo|bar)=([0-9]+)', groupArray(extract(s, '(foo|bar)=([0-9]+)')) FROM strings;
SELECT 'user=(alice|bob)|action=(login|logout)', groupArray(extractAll(s, 'user=(alice|bob)|action=(login|logout)')) FROM strings;

SELECT '-- empty branches and optional groups';
SELECT 'a(|b)c', groupArray(extractAll(s, 'a(|b)c')) FROM strings;
SELECT '(a|)b', groupArray(extractAll(s, '(a|)b')) FROM strings;
SELECT '(?:ab|cd)?e', groupArray(extractAll(s, '(?:ab|cd)?e')) FROM strings;

SELECT '-- separators';
SELECT '\\s*=\\s*| |\\] ', groupArray(extractAll(s, '\\s*=\\s*| |\\] ')) FROM strings;
SELECT ', |; |=', groupArray(extractAll(s, ', |; |=')) FROM strings;
SELECT '(=|:)\\s*', groupArray(extractAll(s, '(=|:)\\s*')) FROM strings;
SELECT '[a-z]+|[0-9]+', groupArray(extractAll(s, '[a-z]+|[0-9]+')) FROM strings;
SELECT '\\d{2,3}|[a-z]{3}', groupArray(extractAll(s, '\\d{2,3}|[a-z]{3}')) FROM strings;
SELECT 'e(a|b|c|d|e|f)', groupArray(extractAll(s, 'e(a|b|c|d|e|f)')) FROM strings;

SELECT '-- match and replaceRegexpAll';
SELECT 'match', groupArray(match(s, 'ab|cd')) FROM strings;
SELECT 'match anchored in group', groupArray(match(s, '^(?:ab|x)')) FROM strings;
SELECT 'replaceRegexpAll', groupArray(replaceRegexpAll(s, '\\s*=\\s*| ', '_')) FROM strings;

DROP TABLE strings;
