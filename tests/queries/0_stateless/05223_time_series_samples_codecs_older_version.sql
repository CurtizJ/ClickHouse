-- A TimeSeries table pinned to a version before 5 keeps the compression codec that version generated for the
-- auto-created `timestamp` column of the samples inner tables (`DoubleDelta, ZSTD(1)` instead of `PFor('double_delta')`),
-- so that a server which doesn't know version 5 can still read the table.

SET allow_experimental_time_series_table = 1;

DROP TABLE IF EXISTS ts_codecs_v4;
CREATE TABLE ts_codecs_v4 ENGINE = TimeSeries SETTINGS version = 4;

SELECT name, type, compression_codec FROM system.columns
WHERE database = currentDatabase() AND table LIKE '.inner\_id.samples.%' ORDER BY position;

DROP TABLE ts_codecs_v4;
