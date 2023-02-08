DROP TABLE IF EXISTS t_tuple_names;

CREATE TABLE t_tuple_names (A UInt64, B UInt64) ENGINE = Memory;
INSERT INTO t_tuple_names VALUES (1, 2);

SELECT tupleToNameValuePairs((A, B)) FROM t_tuple_names;
SELECT tupleToNameValuePairs((A + 1, B + 1)) FROM t_tuple_names;
SELECT tupleToNameValuePairs((3, 2, 1));

DROP TABLE t_tuple_names;
