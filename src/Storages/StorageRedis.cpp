#include "StorageRedis.h"
#include <Common/parseAddress.h>
#include <Interpreters/evaluateConstantExpression.h>

#include <Parsers/ASTLiteral.h>

#include <Storages/StorageFactory.h>

#include "Poco/Redis/Redis.h"
#include "Poco/Redis/Type.h"
#include "Poco/Redis/Exception.h"
#include <Poco/Redis/Array.h>
#include <Poco/Redis/Client.h>
#include <Poco/Redis/Command.h>
#include <Processors/Sinks/SinkToStorage.h>
#include <Common/logger_useful.h>
#include "Dictionaries/RedisSource.h"

namespace DB
{

namespace ErrorCodes
{
    extern const int INTERNAL_REDIS_ERROR;
    extern const int TIMEOUT_EXCEEDED;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int INCORRECT_NUMBER_OF_COLUMNS;
}

StorageRedis::StorageRedis(
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
        const String & comment)
    : IStorage(table_id_)
    , host(host_)
    , port(port_)
    , db_index(db_index_)
    , password(password_)
    , storage_type(storage_type_)
    , context(context_)
    , options(options_)
    , pool(context->getSettingsRef().redis_connection_pool_size)
{
    if (columns_.size() < 2 || columns_.size() > 3)
    {
        throw Exception(ErrorCodes::INCORRECT_NUMBER_OF_COLUMNS,
            "Redis storage supports only either 2 or 3 columns: "
            "key, [field], value. Got: {} columns", columns_.size());
    }

    StorageInMemoryMetadata storage_metadata;
    storage_metadata.setColumns(columns_);
    storage_metadata.setConstraints(constraints_);
    storage_metadata.setComment(comment);
    setInMemoryMetadata(storage_metadata);
}

Pipe StorageRedis::read(
    const Names & column_names,
    const StorageSnapshotPtr & storage_snapshot,
    SelectQueryInfo & /*query_info*/,
    ContextPtr query_context,
    QueryProcessingStage::Enum /*processed_stage*/,
    size_t max_block_size,
    unsigned /*num_streams*/)
{
    auto connection = pool.get(query_context->getSettingsRef().lock_acquire_timeout.totalMilliseconds());
    storage_snapshot->check(column_names);

    const auto & all_columns = storage_snapshot->metadata->getColumns();
    auto sample_block = storage_snapshot->getSampleBlockForColumns(column_names);

    auto keys = Redis::getAllKeys(connection, storage_type, max_block_size);
    std::vector<bool> selected_columns(MAX_COLUMNS);

    auto it = all_columns.begin();
    for (size_t i = 0; i < all_columns.size(); ++i, ++it)
        if (sample_block.has(it->name))
            selected_columns[i] = true;

    return Pipe(std::make_shared<RedisSource>(std::move(connection), keys, storage_type, sample_block, max_block_size, selected_columns));
}

StorageRedisConfiguration StorageRedis::getConfiguration(ASTs engine_args, ContextPtr context)
{
    StorageRedisConfiguration configuration;
    if (auto named_collection = getExternalDataSourceConfiguration(engine_args, context))
    {
        auto [common_configuration, storage_specific_args, _] = named_collection.value();
        configuration.set(common_configuration);

        for (const auto & [arg_name, arg_value] : storage_specific_args)
        {
            if (arg_name == "options")
                configuration.host = arg_value->as<ASTLiteral>()->value.safeGet<String>();
            else
                throw Exception(ErrorCodes::BAD_ARGUMENTS,
                        "Unexpected key-value argument."
                        "Got: {}, but expected one of:"
                        "host, port, database_id, password, storage_type, options.", arg_name);
        }
    }
    else
    {
        if (engine_args.size() < 4 || engine_args.size() > 5)
            throw Exception(
                "Storage Redis requires from 4 to 5 parameters: MongoDB('host:port', 'database_id', 'password', 'storage_type', [, 'options']).",
                ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);


        for (auto & engine_arg : engine_args)
            engine_arg = evaluateConstantExpressionOrIdentifierAsLiteral(engine_arg, context);

        /// 6379 is the default Redis port.
        auto parsed_host_port = parseAddress(engine_args[0]->as<ASTLiteral &>().value.safeGet<String>(), 6379);

        configuration.host = parsed_host_port.first;
        configuration.port = parsed_host_port.second;
        configuration.db_index = engine_args[1]->as<ASTLiteral &>().value.safeGet<UInt32>();
        configuration.password = engine_args[2]->as<ASTLiteral &>().value.safeGet<String>();
        auto st_type = engine_args[3]->as<ASTLiteral &>().value.safeGet<String>();

        configuration.storage_type = StorageRedisConfiguration::Redis::StorageType::UNKNOWN;
        if (st_type == "SIMPLE")
            configuration.storage_type = StorageRedisConfiguration::Redis::StorageType::SIMPLE;
        else if (st_type == "HASH_MAP")
            configuration.storage_type = StorageRedisConfiguration::Redis::StorageType::HASH_MAP;

        if (engine_args.size() >= 5)
            configuration.options = engine_args[4]->as<ASTLiteral &>().value.safeGet<String>();
    }
    return configuration;
}


void registerStorageRedis(StorageFactory & factory)
{
    factory.registerStorage("Redis", [](const StorageFactory::Arguments & args)
    {
        auto configuration = StorageRedis::getConfiguration(args.engine_args, args.getLocalContext());
        std::string primary_key;
        std::string secondary_key;
        std::string value;
        if (configuration.storage_type == StorageRedisConfiguration::Redis::StorageType::HASH_MAP) {
            primary_key = args.columns.getNamesOfPhysical()[0];
            secondary_key = args.columns.getNamesOfPhysical()[1];
            value = args.columns.getNamesOfPhysical()[2];
        } else {
            secondary_key = args.columns.getNamesOfPhysical()[0];
            value = args.columns.getNamesOfPhysical()[1];
        }
        return std::make_shared<StorageRedis>(
            args.table_id,
            configuration.host,
            configuration.port,
            configuration.db_index,
            configuration.password,
            static_cast<Redis::StorageType>(configuration.storage_type),
            primary_key,
            secondary_key,
            value,
            args.getContext(),
            configuration.options,
            args.columns,
            args.constraints,
            args.comment);
    },
    {
        .source_access_type = AccessType::REDIS,
    });
}

}
