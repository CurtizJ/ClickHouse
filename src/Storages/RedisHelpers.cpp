#include <Storages/RedisHelpers.h>
#include <Poco/Redis/Command.h>

namespace DB
{

namespace Redis
{

String storageTypeToKeyType(StorageType type);
{
    switch (type)
    {
        case Redis::StorageType::SIMPLE:
            return "string";
        case Redis::StorageType::HASH_MAP:
            return "hash";
        default:
            return "none";
    }

    __builtin_unreachable();
}

ConnectionPool::ConnectionPool(size_t pool_size_)
    : pool(std::make_shared<Pool>(pool_size_))
{
}

ConnectionPtr ConnectionPool::get(size_t lock_acquire_timeout_ms) const
{
    ClientPtr client;
    bool ok = pool->tryBorrowObject(client,
        [] { return std::make_unique<Poco::Redis::Client>(); },
        lock_acquire_timeout_ms);

    if (!ok)
        throw Exception(ErrorCodes::TIMEOUT_EXCEEDED,
            "Could not get connection from pool, timeout exceeded {} seconds",
            lock_acquire_timeout_ms);

    if (!client->isConnected())
    {
        try
        {
            client->connect(host, port);
            if (!password.empty())
            {
                RedisCommand command("AUTH");
                command << password;
                String reply = client->execute<String>(command);
                if (reply != "OK")
                    throw Exception(ErrorCodes::INTERNAL_REDIS_ERROR,
                        "Authentication failed with reason {}", reply);
            }

            if (db_index != 0)
            {
                RedisCommand command("SELECT");
                command << std::to_string(db_index);
                String reply = client->execute<String>(command);
                if (reply != "OK")
                    throw Exception(ErrorCodes::INTERNAL_REDIS_ERROR,
                        "Selecting database with index {} failed with reason {}",
                        db_index, reply);
            }
        }
        catch (...)
        {
            if (client->isConnected())
                client->disconnect();

            pool->returnObject(std::move(client));
            throw;
        }
    }

    return std::make_unique<Connection>(pool, std::move(client));
}

Poco::Redis::Array getAllKeys(
    const ConnectionPtr & connection,
    StorageType storage_type,
    size_t max_block_size)
{
    Poco::Redis::Command command_for_keys("KEYS");
    command_for_keys << "*";

    auto all_keys = connection->client->execute<Poco::Redis::Array>(command_for_keys);
    if (all_keys.isNull())
        return Poco::Redis::Array{};

    Poco::Redis::Array keys;
    auto key_type = storageTypeToKeyType(storage_type);
    for (auto && key : all_keys)
        if (key_type == connection->client->execute<String>(Poco::Redis::Command("TYPE").addRedisType(key)))
                keys.addRedisType(key);

    if (storage_type == StorageType::HASH_MAP)
    {
        Poco::Redis::Array hkeys;
        for (const auto & key : keys)
        {
            Poco::Redis::Command command_for_secondary_keys("HKEYS");
            command_for_secondary_keys.addRedisType(key);

            auto secondary_keys = connection->client->execute<Poco::Redis::Array>(command_for_secondary_keys);

            Poco::Redis::Array primary_with_secondary;
            primary_with_secondary.addRedisType(key);
            for (const auto & secondary_key : secondary_keys)
            {
                primary_with_secondary.addRedisType(secondary_key);
                if (primary_with_secondary.size() == max_block_size + 1)
                {
                    hkeys.add(primary_with_secondary);
                    primary_with_secondary.clear();
                    primary_with_secondary.addRedisType(key);
                }
            }

            if (primary_with_secondary.size() > 1)
                hkeys.add(primary_with_secondary);
        }

        keys = hkeys;
    }

    return keys;
}

}

}
