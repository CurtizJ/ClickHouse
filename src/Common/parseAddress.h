#pragma once

#include <string>
#include <map>
#include <base/types.h>


namespace DB
{

/** Parse address from string, that can contain host with or without port.
  * If port was not specified and default_port is not zero, default_port is used.
  * Otherwise, an exception is thrown.
  *
  * Examples:
  *  clickhouse.com - returns "clickhouse.com" and default_port
  *  clickhouse.com:80 - returns "clickhouse.com" and 80
  *  [2a02:6b8:a::a]:80 - returns [2a02:6b8:a::a] and 80; note that square brackets remain in returned host.
  */
std::pair<std::string, UInt16> parseAddress(const std::string & str, UInt16 default_port);

/** Extract host and port from a broker connection string that may carry a URI scheme, userinfo and
  * path, as accepted by message-broker engines (for example `nats://user:pass@host:4222/`), or be a
  * bare `host:port` / `host`. The scheme, userinfo and path are stripped and the remaining authority
  * is passed to parseAddress with default_port. An empty authority (for example `nats://`) resolves
  * to `localhost`, matching the client library default. Intended for RemoteHostFilter checks.
  */
std::pair<std::string, UInt16> parseAddressFromURL(const std::string & url, UInt16 default_port);

}
