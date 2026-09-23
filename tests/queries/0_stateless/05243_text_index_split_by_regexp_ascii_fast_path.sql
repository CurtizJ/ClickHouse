-- The `splitByRegexp` tokenizer uses the JIT-compiled matcher on valid UTF-8 when possible. Check that it produces
-- the same tokens as RE2 on ASCII strings, valid UTF-8 and invalid UTF-8.

-- The reference functions must use RE2: their JIT-compiled matcher may differ from it on invalid UTF-8.
SET compile_regular_expressions = 0;

DROP TABLE IF EXISTS tab;

CREATE TABLE tab (s String) ENGINE = Memory;
INSERT INTO tab VALUES
    (''), ('   '), ('abc'), ('[INFO] user=alice ip=10.0.0.1 status=200'), (',,a,,b,,'), ('a=b;c = d'),
    ('trace_id=4bf92f3577b34da6 GET /api/v1/items?id=42'), ('tab\there\nnewline'),
    ('Überprüfung=fehlgeschlagen ÿ€ x'), ('a\xFFb=c'), ('ABC abc AbC'), ('\xFE\xE2\x82]x1 hello'), ('ok \xC3'), ('\xED\xA0\x80 = x');

SELECT '[^A-Za-z0-9._-]+', countIf(tokens(s, 'splitByRegexp', '[^A-Za-z0-9._-]+') != arrayFilter(x -> x != '', splitByRegexp('[^A-Za-z0-9._-]+', s))) FROM tab;
SELECT '[^A-Za-z0-9._-]', countIf(tokens(s, 'splitByRegexp', '[^A-Za-z0-9._-]') != arrayFilter(x -> x != '', splitByRegexp('[^A-Za-z0-9._-]', s))) FROM tab;
SELECT '[ =,;]', countIf(tokens(s, 'splitByRegexp', '[ =,;]') != arrayFilter(x -> x != '', splitByRegexp('[ =,;]', s))) FROM tab;
SELECT '[ =,;]+?', countIf(tokens(s, 'splitByRegexp', '[ =,;]+?') != arrayFilter(x -> x != '', splitByRegexp('[ =,;]+?', s))) FROM tab;
SELECT '[ =,;]{1,2}', countIf(tokens(s, 'splitByRegexp', '[ =,;]{1,2}') != arrayFilter(x -> x != '', splitByRegexp('[ =,;]{1,2}', s))) FROM tab;
SELECT '\\s+', countIf(tokens(s, 'splitByRegexp', '\\s+') != arrayFilter(x -> x != '', splitByRegexp('\\s+', s))) FROM tab;
SELECT '\\W+', countIf(tokens(s, 'splitByRegexp', '\\W+') != arrayFilter(x -> x != '', splitByRegexp('\\W+', s))) FROM tab;
SELECT '(?i)[a-c]+', countIf(tokens(s, 'splitByRegexp', '(?i)[a-c]+') != arrayFilter(x -> x != '', splitByRegexp('(?i)[a-c]+', s))) FROM tab;
SELECT '(\\s+)', countIf(tokens(s, 'splitByRegexp', '(\\s+)') != arrayFilter(x -> x != '', splitByRegexp('(\\s+)', s))) FROM tab;
SELECT '.', countIf(tokens(s, 'splitByRegexp', '.') != arrayFilter(x -> x != '', splitByRegexp('.', s))) FROM tab;
SELECT '[^\\p{L}\\p{N}]+', countIf(tokens(s, 'splitByRegexp', '[^\\p{L}\\p{N}]+') != arrayFilter(x -> x != '', splitByRegexp('[^\\p{L}\\p{N}]+', s))) FROM tab;

SELECT '\\s*=\\s*| |\\] ', countIf(tokens(s, 'splitByRegexp', '\\s*=\\s*| |\\] ') != arrayFilter(x -> x != '', splitByRegexp('\\s*=\\s*| |\\] ', s))) FROM tab;
SELECT 'a|ab', countIf(tokens(s, 'splitByRegexp', 'a|ab') != arrayFilter(x -> x != '', splitByRegexp('a|ab', s))) FROM tab;
SELECT '\\bid\\b', countIf(tokens(s, 'splitByRegexp', '\\bid\\b') != arrayFilter(x -> x != '', splitByRegexp('\\bid\\b', s))) FROM tab;
SELECT '(?i)\\x{212A}', countIf(tokens(s, 'splitByRegexp', '(?i)\\x{212A}') != arrayFilter(x -> x != '', splitByRegexp('(?i)\\x{212A}', s))) FROM tab;
SELECT '^\\s*\\[', countIf(tokens(s, 'splitByRegexp', '^\\s*\\[') != arrayFilter(x -> x != '', splitByRegexp('^\\s*\\[', s))) FROM tab;

SELECT '[A-Za-z0-9._-]+', countIf(tokens(s, 'splitByRegexp', '[A-Za-z0-9._-]+', true) != extractAll(s, '[A-Za-z0-9._-]+')) FROM tab;
SELECT '[a-z]+', countIf(tokens(s, 'splitByRegexp', '[a-z]+', true) != extractAll(s, '[a-z]+')) FROM tab;
SELECT '(?i)[a-c]+', countIf(tokens(s, 'splitByRegexp', '(?i)[a-c]+', true) != extractAll(s, '(?i)[a-c]+')) FROM tab;
SELECT '([a-z0-9]+)', countIf(tokens(s, 'splitByRegexp', '([a-z0-9]+)', true) != extractAll(s, '([a-z0-9]+)')) FROM tab;
SELECT '[a-z]{1,}', countIf(tokens(s, 'splitByRegexp', '[a-z]{1,}', true) != extractAll(s, '[a-z]{1,}')) FROM tab;
SELECT '[a-z]+?', countIf(tokens(s, 'splitByRegexp', '[a-z]+?', true) != extractAll(s, '[a-z]+?')) FROM tab;
SELECT '[a-z]{1,3}', countIf(tokens(s, 'splitByRegexp', '[a-z]{1,3}', true) != extractAll(s, '[a-z]{1,3}')) FROM tab;
SELECT '\\w+', countIf(tokens(s, 'splitByRegexp', '\\w+', true) != extractAll(s, '\\w+')) FROM tab;
SELECT '[\\p{L}\\p{N}]+', countIf(tokens(s, 'splitByRegexp', '[\\p{L}\\p{N}]+', true) != extractAll(s, '[\\p{L}\\p{N}]+')) FROM tab;
SELECT '([a-z]+)=', countIf(tokens(s, 'splitByRegexp', '([a-z]+)=', true) != extractAll(s, '([a-z]+)=')) FROM tab;
SELECT '\\d+\\.\\d+', countIf(tokens(s, 'splitByRegexp', '\\d+\\.\\d+', true) != extractAll(s, '\\d+\\.\\d+')) FROM tab;
SELECT 'ab|a', countIf(tokens(s, 'splitByRegexp', 'ab|a', true) != extractAll(s, 'ab|a')) FROM tab;
SELECT '(?i)\\x{17F}+', countIf(tokens(s, 'splitByRegexp', '(?i)\\x{17F}+', true) != extractAll(s, '(?i)\\x{17F}+')) FROM tab;

SELECT tokens('[INFO] user=alice ip=10.0.0.1', 'splitByRegexp', '[^A-Za-z0-9._-]+');
SELECT tokens('[INFO] user=alice ip=10.0.0.1', 'splitByRegexp', '[A-Za-z0-9._-]+', true);
SELECT tokens('Überprüfung=fehlgeschlagen ÿ€ x', 'splitByRegexp', '[^A-Za-z0-9._-]+');

DROP TABLE tab;

CREATE TABLE tab
(
    id UInt32,
    message String,
    INDEX idx message TYPE text(tokenizer = splitByRegexp('[^A-Za-z0-9._-]+'))
)
ENGINE = MergeTree ORDER BY id SETTINGS index_granularity = 1;

INSERT INTO tab VALUES (1, '[INFO] user=alice ip=10.0.0.1'), (2, '[ERROR] db-01.internal timeout'), (3, 'Überprüfung=fehlgeschlagen host=db-01.internal');

SELECT groupArray(id) FROM tab WHERE hasAnyTokens(message, ['db-01.internal']);
SELECT groupArray(id) FROM tab WHERE hasAllTokens(message, ['alice', '10.0.0.1']);
SELECT groupArray(id) FROM tab WHERE hasAnyTokens(message, ['fehlgeschlagen']);

DROP TABLE tab;
