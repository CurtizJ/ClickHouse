"""
`ENGINE = Kafka` must check every broker in `kafka_broker_list` against `remote_url_allow_hosts`.

The broker list is a comma-separated list of `host:port` endpoints. `CREATE TABLE` runs the check in
the storage constructor, before librdkafka connects, so a disallowed broker is rejected synchronously
with `UNACCEPTABLE_URL`. Every entry is checked, so a single disallowed broker in an otherwise allowed
list is rejected too.
"""

import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)
node = cluster.add_instance(
    "node",
    main_configs=["configs/allowed_hosts.xml"],
    stay_alive=True,
)


@pytest.fixture(scope="module")
def started_cluster():
    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def test_remote_host_filter_applies_to_kafka_broker_list(started_cluster):
    error = node.query_and_get_error(
        """
        CREATE TABLE filtered (key String)
        ENGINE = Kafka
        SETTINGS kafka_broker_list = 'not-allowed-host:9092',
                 kafka_topic_list = 'topic',
                 kafka_group_name = 'group',
                 kafka_format = 'JSONEachRow'
        """
    )
    assert "UNACCEPTABLE_URL" in error, error
    assert "not-allowed-host:9092" in error, error


def test_remote_host_filter_checks_every_broker(started_cluster):
    """A disallowed broker anywhere in the list must be rejected, even alongside an allowed one."""
    error = node.query_and_get_error(
        """
        CREATE TABLE mixed_list (key String)
        ENGINE = Kafka
        SETTINGS kafka_broker_list = 'localhost:19092,not-allowed-host:9092',
                 kafka_topic_list = 'topic',
                 kafka_group_name = 'group',
                 kafka_format = 'JSONEachRow'
        """
    )
    assert "UNACCEPTABLE_URL" in error, error
    assert "not-allowed-host:9092" in error, error
