SET optimize_move_functions_out_of_any = 1;

SELECT sum(number * 2 AS n), any(n) FROM numbers(10);
EXPLAIN SYNTAX SELECT sum(number * 2 AS n), any(n) FROM numbers(10);

SELECT any(2 * 3) FROM numbers(10);
EXPLAIN SYNTAX SELECT any(2 * 3) FROM numbers(10);

DROP TABLE IF EXISTS t_null_if;
CREATE TABLE t_null_if(s String) ENGINE = Memory;

INSERT INTO t_null_if VALUES ('') ('foo');

SELECT any(nullIf(s, '')) FROM t_null_if;
EXPLAIN SYNTAX SELECT any(nullIf(s, '')) FROM t_null_if;

DROP TABLE t_null_if;

DROP TABLE IF EXISTS t_nullable_any;
CREATE TABLE t_nullable_any(a Nullable(UInt32), b Nullable(UInt32)) ENGINE = Memory;

INSERT INTO TABLE t_nullable_any VALUES (1, NULL), (NULL, 2), (NULL, NULL), (3, 4);

SELECT any(tuple(a, b)) FROM t_nullable_any;
EXPLAIN SYNTAX SELECT any(tuple(a, b)) FROM t_nullable_any;

SELECT any(a * b) FROM t_nullable_any;
EXPLAIN SYNTAX SELECT any(a * b) FROM t_nullable_any;

DROP TABLE t_nullable_any;
