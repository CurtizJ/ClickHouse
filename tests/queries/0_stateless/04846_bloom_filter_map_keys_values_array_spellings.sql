-- { echo }

-- The direct array spellings `has`/`indexOf`/`hasAny`/`hasAll` over `mapKeys`/`mapValues` match a
-- `bloom_filter` index on the same expression through the index header, whose `mapKeys`/`mapValues`
-- type has `LowCardinality` stripped. They compare a `FixedString` constant zero-padded, as `equals`
-- does. Cells compare the keyed answer against an unindexed oracle.

DROP TABLE IF EXISTS o_lck;
DROP TABLE IF EXISTS k_lck;
DROP TABLE IF EXISTS o_lcfsv;
DROP TABLE IF EXISTS k_lcfsv;

-- `LowCardinality(String)` keys: `toFixedString('K1', 3)` matches the key with and the key without
-- the trailing zero byte, and the index must agree.
CREATE TABLE o_lck (id UInt64, m Map(LowCardinality(String), UInt8)) ENGINE = Log;
CREATE TABLE k_lck (id UInt64, m Map(LowCardinality(String), UInt8),
    INDEX ik mapKeys(m) TYPE bloom_filter GRANULARITY 1)
ENGINE = MergeTree ORDER BY id SETTINGS index_granularity = 1;
INSERT INTO o_lck VALUES (0, {'K1':1}), (1, {'K1\0':2}), (2, {'X':3});
INSERT INTO k_lck VALUES (0, {'K1':1}), (1, {'K1\0':2}), (2, {'X':3});

SELECT (SELECT count() FROM k_lck WHERE has(mapKeys(m), 'K1')) = (SELECT count() FROM o_lck WHERE has(mapKeys(m), 'K1'));
SELECT (SELECT count() FROM k_lck WHERE has(mapKeys(m), toFixedString('K1', 2))) = (SELECT count() FROM o_lck WHERE has(mapKeys(m), toFixedString('K1', 2)));
SELECT (SELECT count() FROM k_lck WHERE has(mapKeys(m), toFixedString('K1', 3))) = (SELECT count() FROM o_lck WHERE has(mapKeys(m), toFixedString('K1', 3)));
SELECT (SELECT count() FROM k_lck WHERE indexOf(mapKeys(m), toFixedString('K1', 2)) != 0) = (SELECT count() FROM o_lck WHERE indexOf(mapKeys(m), toFixedString('K1', 2)) != 0);
SELECT (SELECT count() FROM k_lck WHERE indexOf(mapKeys(m), toFixedString('K1', 3)) != 0) = (SELECT count() FROM o_lck WHERE indexOf(mapKeys(m), toFixedString('K1', 3)) != 0);
SELECT (SELECT count() FROM k_lck WHERE hasAny(mapKeys(m), [toFixedString('K1', 3)])) = (SELECT count() FROM o_lck WHERE hasAny(mapKeys(m), [toFixedString('K1', 3)]));
SELECT (SELECT count() FROM k_lck WHERE hasAll(mapKeys(m), [toFixedString('K1', 3)])) = (SELECT count() FROM o_lck WHERE hasAll(mapKeys(m), [toFixedString('K1', 3)]));

-- Pin the zero-padded semantics: the padded constant selects both keys.
SELECT id FROM k_lck WHERE has(mapKeys(m), toFixedString('K1', 3)) ORDER BY id;
SELECT id FROM k_lck WHERE hasAny(mapKeys(m), [toFixedString('K1', 3)]) ORDER BY id;

-- `LowCardinality(FixedString(3))` values: an over-wide constant compares without throwing, and
-- matches when only trailing zero bytes make it wider.
CREATE TABLE o_lcfsv (id UInt64, m Map(String, LowCardinality(FixedString(3)))) ENGINE = Log;
CREATE TABLE k_lcfsv (id UInt64, m Map(String, LowCardinality(FixedString(3))),
    INDEX iv mapValues(m) TYPE bloom_filter GRANULARITY 1)
ENGINE = MergeTree ORDER BY id SETTINGS index_granularity = 1;
INSERT INTO o_lcfsv VALUES (0, {'a':'V0'}), (1, {'b':'V1A'}), (2, {'c':'XYZ'});
INSERT INTO k_lcfsv VALUES (0, {'a':'V0'}), (1, {'b':'V1A'}), (2, {'c':'XYZ'});

SELECT (SELECT count() FROM k_lcfsv WHERE has(mapValues(m), 'V0')) = (SELECT count() FROM o_lcfsv WHERE has(mapValues(m), 'V0'));
SELECT (SELECT count() FROM k_lcfsv WHERE has(mapValues(m), toFixedString('V0', 3))) = (SELECT count() FROM o_lcfsv WHERE has(mapValues(m), toFixedString('V0', 3)));
SELECT (SELECT count() FROM k_lcfsv WHERE has(mapValues(m), toFixedString('V0', 5))) = (SELECT count() FROM o_lcfsv WHERE has(mapValues(m), toFixedString('V0', 5)));
SELECT (SELECT count() FROM k_lcfsv WHERE has(mapValues(m), 'LONGCONST')) = (SELECT count() FROM o_lcfsv WHERE has(mapValues(m), 'LONGCONST'));
SELECT (SELECT count() FROM k_lcfsv WHERE hasAny(mapValues(m), ['V0', 'XYZ'])) = (SELECT count() FROM o_lcfsv WHERE hasAny(mapValues(m), ['V0', 'XYZ']));

-- Pin the zero-padded semantics: the short constant matches the padded element.
SELECT id FROM k_lcfsv WHERE has(mapValues(m), 'V0') ORDER BY id;

-- Pruning is preserved where the index can hash exactly: some stage selects more than zero and
-- fewer than all granules.
SELECT count() > 0 FROM (EXPLAIN indexes = 1 SELECT count() FROM k_lck WHERE has(mapKeys(m), 'K1')) WHERE explain LIKE '%Granules: %/%' AND toUInt64OrZero(extract(explain, 'Granules: (\d+)/')) > 0 AND toUInt64OrZero(extract(explain, 'Granules: (\d+)/')) < toUInt64OrZero(extract(explain, 'Granules: \d+/(\d+)'));
SELECT count() > 0 FROM (EXPLAIN indexes = 1 SELECT count() FROM k_lcfsv WHERE has(mapValues(m), toFixedString('V0', 3))) WHERE explain LIKE '%Granules: %/%' AND toUInt64OrZero(extract(explain, 'Granules: (\d+)/')) > 0 AND toUInt64OrZero(extract(explain, 'Granules: (\d+)/')) < toUInt64OrZero(extract(explain, 'Granules: \d+/(\d+)'));

DROP TABLE o_lck;
DROP TABLE k_lck;
DROP TABLE o_lcfsv;
DROP TABLE k_lcfsv;
