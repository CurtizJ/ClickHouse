-- Tags: no-random-detach
-- Tag no-random-detach: flag is_frozen is not persisted.
-- Since, it's almost useless, there is no reason to fix it.

DROP TABLE IF EXISTS part_info;
CREATE TABLE part_info (t DateTime) ENGINE = MergeTree PARTITION BY toDate(t) ORDER BY (t);
INSERT INTO part_info VALUES (toDateTime('1970-10-01 00:00:01')), (toDateTime('1970-10-02 00:00:01')), (toDateTime('1970-10-03 00:00:01'));
SELECT name, is_frozen FROM system.parts WHERE `database` = currentDatabase() AND `table` = 'part_info';
SELECT 'freeze one';
ALTER TABLE part_info FREEZE PARTITION '1970-10-02';
SELECT name, is_frozen FROM system.parts WHERE `database` = currentDatabase() AND `table` = 'part_info';
SELECT 'freeze all';
ALTER TABLE part_info FREEZE;
SELECT name, is_frozen FROM system.parts WHERE `database` = currentDatabase() AND `table` = 'part_info';
INSERT INTO part_info VALUES (toDateTime('1970-10-02 00:00:02'));
select * from part_info order by t;
SELECT name, is_frozen FROM system.parts WHERE `database` = currentDatabase() AND `table` = 'part_info';
DROP TABLE part_info;
