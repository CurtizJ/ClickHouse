#pragma once

#include <Core/Block.h>
#include <base/BorrowedObjectPool.h>
#include <Core/ExternalResultDescription.h>
#include <Storages/ExternalDataSourceConfiguration.h>
#include <Processors/ISource.h>
#include <Poco/Redis/Array.h>
#include <Poco/Redis/Type.h>
#include <Poco/Redis/Client.h>

namespace DB
{

class RedisSource final : public ISource
{
public:
    using RedisArray = Poco::Redis::Array;
    using RedisBulkString = Poco::Redis::BulkString;

    RedisSource(
        Redis::ConnectionPtr connection_,
        const Poco::Redis::Array & keys_,
        const Redis::StorageType & storage_type_,
        const Block & sample_block,
        size_t max_block_size,
        const std::vector<bool> & selected_columns_ = {true, true, true});

    ~RedisSource() override;

    String getName() const override { return "Redis"; }

private:
    Chunk generate() override;

    Redis::ConnectionPtr connection;
    Poco::Redis::Array keys;
    Redis::StorageType storage_type;
    const size_t max_block_size;
    ExternalResultDescription description;
    size_t cursor = 0;
    bool all_read = false;
    const std::vector<bool> selected_columns;
};

}

