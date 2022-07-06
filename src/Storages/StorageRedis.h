#pragma once

#include <Interpreters/Context.h>
#include <Storages/IStorage.h>
#include <Storages/ExternalDataSourceConfiguration.h>
#include <Core/Block.h>
#include <Dictionaries/RedisSource.h>
namespace Poco
{
    namespace Util
    {
        class AbstractConfiguration;
    }

    namespace Redis
    {
        class Client;
        class Array;
        class Command;
    }
}

namespace DB
{

class StorageRedis final : public IStorage
{
public:
    static constexpr size_t MAX_COLUMNS = 3;

    using RedisCommand = Poco::Redis::Command;

    StorageRedis(
        const StorageID & table_id_,
        const std::string & host_,
        const UInt16 & port_,
        const UInt32 & db_index_,
        const std::string & password_,
        const Redis::StorageType & storage_type_,
        ContextPtr context_,
        const std::string & options_,
        const ColumnsDescription & columns_,
        const ConstraintsDescription & constraints_,
        const String & comment);

    String getName() const override { return "Redis"; }

    Pipe read(
        const Names & column_names,
        const StorageSnapshotPtr & storage_snapshot,
        SelectQueryInfo & query_info,
        ContextPtr query_context,
        QueryProcessingStage::Enum processed_stage,
        size_t max_block_size,
        unsigned num_streams) override;

    static StorageRedisConfiguration getConfiguration(ASTs engine_args, ContextPtr context);
private:
    const std::string host;
    const UInt16 port;
    const UInt32 db_index;
    const std::string password;

    Redis::StorageType storage_type;
    ContextPtr context;
    String options;

    Redis::ConnectionPool pool;
    Block header;
};

}
