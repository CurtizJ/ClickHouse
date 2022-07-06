#pragma once

#include <Core/Block.h>
#include "RedisSource.h"
#include "DictionaryStructure.h"
#include "IDictionarySource.h"

namespace DB
{
    namespace ErrorCodes
    {
        extern const int NOT_IMPLEMENTED;
    }

    class RedisDictionarySource final : public IDictionarySource
    {
    public:
        using RedisArray = Poco::Redis::Array;
        using RedisCommand = Poco::Redis::Command;

        struct Configuration
        {
            const std::string host;
            const UInt16 port;
            const UInt32 db_index;
            const std::string password;
            const Redis::StorageType storage_type;
            const size_t pool_size;
        };

        RedisDictionarySource(
            const DictionaryStructure & dict_struct_,
            const Configuration & configuration_,
            const Block & sample_block_);

        RedisDictionarySource(const RedisDictionarySource & other);

        ~RedisDictionarySource() override = default;

        QueryPipeline loadAll() override;

        QueryPipeline loadUpdatedAll() override
        {
            throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Method loadUpdatedAll is unsupported for RedisDictionarySource");
        }

        bool supportsSelectiveLoad() const override { return true; }

        QueryPipeline loadIds(const std::vector<UInt64> & ids) override;

        QueryPipeline loadKeys(const Columns & key_columns, const std::vector<size_t> & requested_rows) override;

        bool isModified() const override { return true; }

        bool hasUpdateField() const override { return false; }

        DictionarySourcePtr clone() const override { return std::make_shared<RedisDictionarySource>(*this); }

        std::string toString() const override;

    private:
        ConnectionPtr getConnection() const;

        const DictionaryStructure dict_struct;
        const Configuration configuration;

        Redis::ConnectionPool pool;
        Block sample_block;
    };
}
