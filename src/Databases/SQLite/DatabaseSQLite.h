#pragma once

#include "config.h"

#if USE_SQLITE
#include <Core/Names.h>
#include <Databases/DatabasesCommon.h>
#include <Parsers/ASTCreateQuery.h>

#include <sqlite3.h>


namespace DB
{
class DatabaseSQLite final : public IDatabase, WithContext
{
public:
    using SQLitePtr = std::shared_ptr<sqlite3>;

    DatabaseSQLite(ContextPtr context_, const ASTStorage * database_engine_define_,
                   bool is_attach_, const String & database_path_);

    String getEngineName() const override { return "SQLite"; }

    bool supportsDetachingTables() const override { return false; }

    bool canContainMergeTreeTables() const override { return false; }

    bool canContainDistributedTables() const override { return false; }

    bool shouldBeEmptyOnDetach() const override { return false; }

    bool isTableExist(const String & name, ContextPtr context) const override;

    StoragePtr tryGetTable(const String & name, ContextPtr context) const override;

    DatabaseTablesIteratorPtr getTablesIterator(ContextPtr context, const FilterByNameFunction & filter_by_table_name) const override;

    bool empty() const override;

    ASTPtr getCreateDatabaseQuery() const override;

    void shutdown() override {}

protected:
    ASTPtr getCreateTableQueryImpl(const String & table_name, ContextPtr context, bool throw_on_error) const override;

private:
    ASTPtr database_engine_define;

    String database_path;

    mutable SQLitePtr sqlite_db;

    Poco::Logger * log;

    bool checkSQLiteTable(const String & table_name) const;

    NameSet fetchTablesList() const TSA_REQUIRES(mutex);

    StoragePtr fetchTable(const String & table_name, ContextPtr context, bool table_checked) const TSA_REQUIRES(mutex);

};

}

#endif
