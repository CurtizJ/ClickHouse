#include "ActionLocksManager.h"
#include <Interpreters/Context.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Databases/IDatabase.h>
#include <Storages/IStorage.h>


namespace DB
{

namespace ActionLocks
{
    extern const StorageActionBlockType PartsMerge = 1;
    extern const StorageActionBlockType PartsFetch = 2;
    extern const StorageActionBlockType PartsSend = 3;
    extern const StorageActionBlockType ReplicationQueue = 4;
    extern const StorageActionBlockType DistributedSend = 5;
    extern const StorageActionBlockType PartsTTLMerge = 6;
    extern const StorageActionBlockType PartsMove = 7;
    extern const StorageActionBlockType PullReplicationLog = 8;
    extern const StorageActionBlockType Cleanup = 9;
    extern const StorageActionBlockType ViewRefresh = 10;
}


ActionLocksManager::ActionLocksManager(ContextPtr context_) : WithContext(context_->getGlobalContext())
{
}

void ActionLocksManager::add(const StorageID & table_id, StorageActionBlockType action_type)
{
    if (auto table = DatabaseCatalog::instance().tryGetTable(table_id, getContext()))
        add(table, action_type);
}

void ActionLocksManager::add(const StoragePtr & table, StorageActionBlockType action_type)
{
    ActionLock action_lock = table->getActionLock(action_type);

    if (!action_lock.expired())
    {
        std::lock_guard lock(mutex);
        storage_locks[table.get()][action_type] = std::move(action_lock);
    }
}

void ActionLocksManager::remove(const StorageID & table_id, StorageActionBlockType action_type)
{
    if (auto table = DatabaseCatalog::instance().tryGetTable(table_id, getContext()))
        remove(table, action_type);
}

void ActionLocksManager::remove(const StoragePtr & table, StorageActionBlockType action_type)
{
    std::lock_guard lock(mutex);

    if (storage_locks.contains(table.get()))
        storage_locks[table.get()].erase(action_type);
}

bool ActionLocksManager::has(const StorageID & table_id, StorageActionBlockType action_type) const
{
    if (auto table = DatabaseCatalog::instance().tryGetTable(table_id, getContext()))
        return has(table, action_type);
    return false;
}

bool ActionLocksManager::has(const StoragePtr & table, StorageActionBlockType action_type) const
{
    std::lock_guard lock(mutex);

    auto it = storage_locks.find(table.get());
    if (it == storage_locks.end())
        return false;

    return it->second.contains(action_type);
}

bool ActionLocksManager::hasAny(const StorageID & table_id) const
{
    if (auto table = DatabaseCatalog::instance().tryGetTable(table_id, getContext()))
        return hasAny(table);
    return false;
}

bool ActionLocksManager::hasAny(const StoragePtr & table) const
{
    std::lock_guard lock(mutex);
    return storage_locks.contains(table.get());
}

void ActionLocksManager::cleanExpired()
{
    std::lock_guard lock(mutex);

    for (auto it_storage = storage_locks.begin(); it_storage != storage_locks.end();)
    {
        auto & locks = it_storage->second;
        for (auto it_lock = locks.begin(); it_lock != locks.end();)
        {
            if (it_lock->second.expired())
                it_lock = locks.erase(it_lock);
            else
                ++it_lock;
        }

        if (locks.empty())
            it_storage = storage_locks.erase(it_storage);
        else
            ++it_storage;
    }
}

}
