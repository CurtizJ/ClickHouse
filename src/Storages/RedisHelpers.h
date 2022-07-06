#pragma once
#include <Poco/Redis/Client.h>
#include <base/BorrowedObjectPool.h>

namespace DB
{

namespace Redis
{

using ClientPtr = std::unique_ptr<Poco::Redis::Client>;

namespace
{

using Pool = BorrowedObjectPool<ClientPtr>;
using PoolPtr = std::shared_ptr<Pool>;

}

class Connection
{
public:
    Connection(PoolPtr pool_, ClientPtr client_)
        : client(std::move(client_)), pool(std::move(pool_))
    {
    }

    ~Connection()
    {
        pool->returnObject(std::move(client));
    }

    ClientPtr client;

private:
    PoolPtr pool;
};

using ConnectionPtr = std::unique_ptr<Connection>;

class ConnectionPool
{
public:
    ConnectionPool(size_t pool_size_);
    ConnectionPtr get(size_t lock_acquire_timeout_ms) const;

private:
    PoolPtr pool;
};

enum class StorageType
{
    SIMPLE,
    HASH_MAP,
    UNKNOWN
};

String storageTypeToKeyType(StorageType type);

Poco::Redis::Array getAllKeys(
    const ConnectionPtr & connection,
    StorageType storage_type,
    size_t max_block_size);

}

}
