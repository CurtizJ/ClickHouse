-- `has(<constant array>, <indexed scalar>)` compares zero-padded: an over-wide
-- `FixedString` array element matches a narrower scalar when only trailing zero
-- bytes make it wider, and bloom-filter analysis must not throw while it
-- materializes the element in the scalar's type.
CREATE TABLE k_fixed
(
    s FixedString(3),
    INDEX idx s TYPE bloom_filter GRANULARITY 1
)
ENGINE = MergeTree
ORDER BY tuple();

CREATE TABLE k_low_cardinality
(
    s LowCardinality(FixedString(3)),
    INDEX idx s TYPE bloom_filter GRANULARITY 1
)
ENGINE = MergeTree
ORDER BY tuple();

CREATE TABLE k_nullable
(
    s Nullable(FixedString(3)),
    INDEX idx s TYPE bloom_filter GRANULARITY 1
)
ENGINE = MergeTree
ORDER BY tuple();

INSERT INTO k_fixed VALUES ('V0');
INSERT INTO k_low_cardinality VALUES ('V0');
INSERT INTO k_nullable VALUES ('V0');

SELECT count() FROM k_fixed WHERE has([toFixedString('V0', 5)], s);
SELECT count() FROM k_low_cardinality WHERE has([toFixedString('V0', 5)], s);
SELECT count() FROM k_nullable WHERE has([toFixedString('V0', 5)], s);

DROP TABLE k_fixed;
DROP TABLE k_low_cardinality;
DROP TABLE k_nullable;
