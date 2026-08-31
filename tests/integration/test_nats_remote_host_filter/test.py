"""
`ENGINE = NATS` must check its broker destination against `remote_url_allow_hosts`.

Both `nats_url` and `nats_server_list` name a broker, and `NATSConnection` prefers `nats_url` when
both are set. Only checking the effective one would let the other smuggle a disallowed host past the
filter, so both are validated whenever set. `CREATE TABLE` runs the check in the storage constructor,
before any connection, so a disallowed host is rejected synchronously with `UNACCEPTABLE_URL`.
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


def test_remote_host_filter_applies_to_nats_url(started_cluster):
    error = node.query_and_get_error(
        """
        CREATE TABLE filtered (key String)
        ENGINE = NATS
        SETTINGS nats_url = 'nats://not-allowed-host:4222/',
                 nats_subjects = 'subject',
                 nats_format = 'JSONEachRow'
        """
    )
    assert "UNACCEPTABLE_URL" in error, error
    assert "not-allowed-host:4222" in error, error


def test_remote_host_filter_applies_to_nats_server_list(started_cluster):
    error = node.query_and_get_error(
        """
        CREATE TABLE filtered_list (key String)
        ENGINE = NATS
        SETTINGS nats_server_list = 'not-allowed-host:4222',
                 nats_subjects = 'subject',
                 nats_format = 'JSONEachRow'
        """
    )
    assert "UNACCEPTABLE_URL" in error, error
    assert "not-allowed-host:4222" in error, error


def test_remote_host_filter_not_bypassed_by_server_list(started_cluster):
    """An allowed `nats_server_list` must not smuggle a disallowed `nats_url` past the filter.

    `NATSConnection` connects to `nats_url` in preference to `nats_server_list`, so validating only
    the server list let an allowed `nats_server_list` (`localhost:4444` is in the allowlist) pair
    with a disallowed `nats_url` and still reach the unvalidated host.
    """
    error = node.query_and_get_error(
        """
        CREATE TABLE both_settings (key String)
        ENGINE = NATS
        SETTINGS nats_server_list = 'localhost:4444',
                 nats_url = 'nats://not-allowed-host:4222/',
                 nats_subjects = 'subject',
                 nats_format = 'JSONEachRow'
        """
    )
    assert "UNACCEPTABLE_URL" in error, error
    assert "not-allowed-host:4222" in error, error
