-- Basic semantics: only the keys that exist in both maps contribute.
SELECT '-- basic';
SELECT mapDotProduct(map('a', 1, 'b', 2, 'c', 3), map('b', 10, 'c', 20, 'd', 30)) AS res, toTypeName(res);
SELECT mapDotProduct(map('a', 1), map('b', 1));
SELECT mapDotProduct(CAST(map(), 'Map(String, Float32)'), map('a', 1.5));
SELECT mapDotProduct(map('x', 0.5, 'y', 1.5), map('x', 2, 'y', 4, 'z', 100));
SELECT mapDotProduct(map('a', -1, 'b', 2), map('a', 3, 'b', -4));

-- The result type is inferred as in arrayDotProduct.
SELECT '-- result types';
SELECT toTypeName(mapDotProduct(map('a', 1::Float32), map('a', 2::Float32)));
SELECT toTypeName(mapDotProduct(map('a', 1::Float32), map('a', 2::Float64)));
SELECT toTypeName(mapDotProduct(map('a', 1::BFloat16), map('a', 2::BFloat16)));
SELECT toTypeName(mapDotProduct(map('a', 1::BFloat16), map('a', 2::Float32)));
SELECT toTypeName(mapDotProduct(map('a', 1::UInt8), map('a', 2::BFloat16)));
SELECT toTypeName(mapDotProduct(map('a', 1::UInt8), map('a', 2::UInt8)));
SELECT toTypeName(mapDotProduct(map('a', 1::UInt32), map('a', 2::UInt32)));
SELECT toTypeName(mapDotProduct(map('a', 1::Int32), map('a', 2::UInt64)));
SELECT toTypeName(mapDotProduct(map('a', 1::Int8), map('a', 2::UInt8)));
SELECT mapDotProduct(map('a', 3::BFloat16, 'b', 4::BFloat16), map('a', 2::BFloat16, 'b', 0.5::BFloat16));
SELECT mapDotProduct(map('a', 1::Int64), map('a', -1::Int64));

-- Integer keys of different widths are converted to a common type.
SELECT '-- integer keys';
SELECT mapDotProduct(map(1::UInt8, 10, 2::UInt8, 20), map(1::UInt32, 3, 2::UInt32, 4));
SELECT mapDotProduct(map(-1::Int8, 10, 2::Int8, 20), map(-1::Int64, 3, 2::Int64, 4));
SELECT mapDotProduct(map(255::UInt8, 10), map(-1::Int8, 3));
SELECT mapDotProduct(map(255::UInt8, 10), map(255::Int16, 3));
SELECT mapDotProduct(map(0::UInt64, 10, 18446744073709551615::UInt64, 20), map(18446744073709551615::UInt64, 3));

-- String and FixedString keys.
SELECT '-- string keys';
SELECT mapDotProduct(map('ab'::FixedString(3), 1, 'cd'::FixedString(3), 2), map('ab'::FixedString(3), 10, 'cd'::FixedString(3), 20));
SELECT mapDotProduct(map('ab'::FixedString(3), 1, 'cd'::FixedString(3), 2), map('ab', 10, 'cd', 20));
SELECT mapDotProduct(map('ab'::FixedString(2), 1), map('ab'::FixedString(3), 10));
SELECT mapDotProduct(map('', 1, 'a', 2), map('', 10, 'a', 20));
SELECT mapDotProduct(CAST(map('a', 1, 'b', 2), 'Map(LowCardinality(String), UInt8)'), map('b', 10, 'c', 20));
SELECT mapDotProduct(map('b', 10, 'c', 20), CAST(map('a', 1, 'b', 2), 'Map(LowCardinality(String), UInt8)'));
SELECT mapDotProduct(CAST(map('a', 1, 'b', 2), 'Map(LowCardinality(String), UInt8)'), CAST(map('b', 10, 'c', 20), 'Map(LowCardinality(String), UInt8)'));

-- The values of repeated keys are summed up, so the result does not depend on the order of arguments.
SELECT '-- repeated keys';
SELECT mapDotProduct(map('a', 1, 'a', 2), map('a', 10));
SELECT mapDotProduct(map('a', 10), map('a', 1, 'a', 2));
SELECT mapDotProduct(map('a', 1, 'a', 2), map('a', 10, 'a', 5));

SELECT '-- errors';
SELECT mapDotProduct(map('a', 1)); -- { serverError NUMBER_OF_ARGUMENTS_DOESNT_MATCH }
SELECT mapDotProduct(map('a', 1), [1]); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
SELECT mapDotProduct(map('a', 1), map(1, 1)); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
SELECT mapDotProduct(map(1.5, 1), map(1.5, 1)); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
SELECT mapDotProduct(map('a', 'x'), map('a', 1)); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
SELECT mapDotProduct(map('a', 1), map('a', 1::Nullable(UInt8))); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
SELECT mapDotProduct(map('a', 1), map('a', 1::Int128)); -- { serverError ILLEGAL_TYPE_OF_ARGUMENT }
SELECT mapDotProduct(map(1::Int64, 1), map(1::UInt64, 1)); -- { serverError NO_COMMON_TYPE }

-- Columns against constants and against each other.
DROP TABLE IF EXISTS t_map_dot_product;

CREATE TABLE t_map_dot_product
(
    id UInt32,
    m_str Map(String, Float32),
    m_lc Map(LowCardinality(String), Float32),
    m_int_values Map(String, UInt8),
    m_int_keys Map(UInt32, Int32)
)
ENGINE = MergeTree ORDER BY id;

INSERT INTO t_map_dot_product VALUES
    (1, map('a', 1.0, 'b', 2.0, 'c', 3.0), map('a', 1.0, 'b', 2.0, 'c', 3.0), map('a', 1, 'b', 2, 'c', 3), map(1, 1, 2, 2, 3, 3)),
    (2, map('b', 0.5), map('b', 0.5), map('b', 5), map(2, 5)),
    (3, map(), map(), map(), map()),
    (4, map('c', -1.0, 'd', 4.0), map('c', -1.0, 'd', 4.0), map('c', 1, 'd', 4), map(3, -1, 4, 4)),
    (5, map('a', 10.0, 'a', 1.0), map('a', 10.0, 'a', 1.0), map('a', 10, 'a', 1), map(1, 10, 1, 1));

SELECT '-- column and constant';
SELECT id, mapDotProduct(m_str, map('a', 2.0, 'c', 1.0)) AS res, toTypeName(res) FROM t_map_dot_product ORDER BY id;
SELECT id, mapDotProduct(map('a', 2.0, 'c', 1.0), m_str) AS res FROM t_map_dot_product ORDER BY id;
SELECT id, mapDotProduct(m_str, map('a', 2::Float32, 'c', 1::Float32)) AS res, toTypeName(res) FROM t_map_dot_product ORDER BY id;
SELECT id, mapDotProduct(m_int_values, map('a', 2, 'c', 1)) AS res, toTypeName(res) FROM t_map_dot_product ORDER BY id;
SELECT id, mapDotProduct(m_int_values, map('a', 0.5, 'c', 1.5)) AS res, toTypeName(res) FROM t_map_dot_product ORDER BY id;
SELECT id, mapDotProduct(m_int_keys, map(1, 1, 3, 2)) AS res, toTypeName(res) FROM t_map_dot_product ORDER BY id;
SELECT id, mapDotProduct(m_int_keys, map(1::Int64, 1, 3::Int64, 2)) AS res, toTypeName(res) FROM t_map_dot_product ORDER BY id;
SELECT id, mapDotProduct(m_int_keys, map(4::Int8, 1, 3::Int8, 2)) AS res FROM t_map_dot_product ORDER BY id;

SELECT '-- low cardinality column and constant';
SELECT id, mapDotProduct(m_lc, map('a', 2.0, 'c', 1.0)) AS res, toTypeName(res) FROM t_map_dot_product ORDER BY id;
SELECT id, mapDotProduct(map('a', 2.0, 'c', 1.0), m_lc) AS res FROM t_map_dot_product ORDER BY id;
SELECT id, mapDotProduct(m_lc, CAST(map('a', 2.0, 'c', 1.0), 'Map(LowCardinality(String), Float64)')) AS res FROM t_map_dot_product ORDER BY id;
SELECT id, mapDotProduct(m_lc, map('a'::FixedString(1), 2.0, 'c'::FixedString(1), 1.0)) AS res FROM t_map_dot_product ORDER BY id;
SELECT id, mapDotProduct(m_lc, map('a', 2.0, 'c', 1.0)) AS res FROM t_map_dot_product WHERE id = 1;
SELECT id, mapDotProduct(m_lc, map('a', 2.0, 'c', 1.0)) AS res FROM t_map_dot_product WHERE id = 3;

SELECT '-- two columns';
SELECT id, mapDotProduct(m_str, m_int_values) AS res, toTypeName(res) FROM t_map_dot_product ORDER BY id;
SELECT id, mapDotProduct(m_str, m_str) AS res, toTypeName(res) FROM t_map_dot_product ORDER BY id;
SELECT id, mapDotProduct(m_lc, m_str) AS res FROM t_map_dot_product ORDER BY id;
SELECT id, mapDotProduct(m_lc, m_lc) AS res FROM t_map_dot_product ORDER BY id;
SELECT id, mapDotProduct(m_int_keys, m_int_keys) AS res, toTypeName(res) FROM t_map_dot_product ORDER BY id;
SELECT id, mapDotProduct(m_str, materialize(map('a', 2.0, 'c', 1.0))) AS res FROM t_map_dot_product ORDER BY id;
SELECT id, mapDotProduct(materialize(map(1, 1, 3, 2)), m_int_keys) AS res FROM t_map_dot_product ORDER BY id;

SELECT '-- scoring sparse vectors';
SELECT id, mapDotProduct(m_str, map('a', 1.0, 'b', 0.5, 'd', 0.25)) AS score
FROM t_map_dot_product
WHERE score > 0
ORDER BY score DESC, id
LIMIT 3;

DROP TABLE t_map_dot_product;

-- Cross-check against a reference expression on random maps without repeated keys.
SELECT '-- cross-check';
SELECT count()
FROM
(
    SELECT
        mapFromArrays(arrayMap(k -> toString(k), keys1), arrayMap(k -> toInt32(cityHash64(number, k, 1) % 21) - 10, keys1)) AS m1,
        mapFromArrays(arrayMap(k -> toString(k), keys2), arrayMap(k -> toInt32(cityHash64(number, k, 2) % 21) - 10, keys2)) AS m2,
        CAST(m1, 'Map(LowCardinality(String), Int32)') AS m1_lc,
        mapFromArrays(keys1, mapValues(m1)) AS m1_int,
        mapFromArrays(keys2, mapValues(m2)) AS m2_int,
        map('1', 3, '5', -2, '7', 10, '20', 1, '49', -7) AS m_const,
        arraySum(k -> m1[k] * m2[k], arrayIntersect(mapKeys(m1), mapKeys(m2))) AS expected,
        arraySum(k -> m1[k] * m_const[k], arrayIntersect(mapKeys(m1), mapKeys(m_const))) AS expected_const
    FROM
    (
        SELECT
            number,
            arrayFilter(k -> cityHash64(number, k, 3) % 3 = 0, range(50)) AS keys1,
            arrayFilter(k -> cityHash64(number, k, 4) % 2 = 0, range(50)) AS keys2
        FROM numbers(2000)
    )
)
WHERE mapDotProduct(m1, m2) != expected
    OR mapDotProduct(m2, m1) != expected
    OR mapDotProduct(m1_lc, m2) != expected
    OR mapDotProduct(m1_int, m2_int) != expected
    OR mapDotProduct(m1, m_const) != expected_const
    OR mapDotProduct(m_const, m1) != expected_const
    OR mapDotProduct(m1_lc, m_const) != expected_const
    OR mapDotProduct(m_const, m1_lc) != expected_const;
