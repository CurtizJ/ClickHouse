#include <Storages/MergeTree/MergeTreeIndexPrefetch.h>

#include <Common/ElapsedTimeProfileEventIncrement.h>
#include <Common/FailPoint.h>
#include <Common/ProfileEvents.h>
#include <Common/setThreadName.h>
#include <Common/ZooKeeper/ZooKeeperCommon.h>
#include <Storages/MergeTree/IDataPartStorage.h>
#include <Storages/MergeTree/IMergeTreeDataPartInfoForReader.h>
#include <Storages/MergeTree/MergeTreeIOSettings.h>
#include <Storages/MergeTree/MergeTreeIndexGranularity.h>
#include <Storages/MergeTree/MergeTreeIndexReader.h>

#include <utility>

namespace ProfileEvents
{
    extern const Event SkipIndexPrefetches;
    extern const Event SkipIndexPrefetchesRunInline;
    extern const Event SkipIndexPrefetchWaitMicroseconds;
    extern const Event SkipIndexPrefetchBudgetExhausted;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int FAULT_INJECTED;
}

namespace FailPoints
{
    extern const char skip_index_prefetch_job_exception[];
}

IndexPrefetchBudget::IndexPrefetchBudget(size_t max_prefetches_, size_t max_bytes_)
    : max_prefetches(max_prefetches_)
    , max_bytes(max_bytes_)
{
}

bool IndexPrefetchBudget::tryAcquire(size_t bytes)
{
    if (num_prefetches.fetch_add(1, std::memory_order_relaxed) >= max_prefetches && max_prefetches)
    {
        num_prefetches.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }

    if (num_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes > max_bytes)
    {
        num_bytes.fetch_sub(bytes, std::memory_order_relaxed);
        num_prefetches.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }

    return true;
}

void IndexPrefetchBudget::release(size_t bytes)
{
    num_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    num_prefetches.fetch_sub(1, std::memory_order_relaxed);
}

IndexPrefetchBudgetSlot::IndexPrefetchBudgetSlot(IndexPrefetchBudgetSlot && other) noexcept
    : budget(std::exchange(other.budget, nullptr))
    , bytes(std::exchange(other.bytes, 0))
{
}

IndexPrefetchBudgetSlot & IndexPrefetchBudgetSlot::operator=(IndexPrefetchBudgetSlot && other) noexcept
{
    if (this != &other)
    {
        reset();
        budget = std::exchange(other.budget, nullptr);
        bytes = std::exchange(other.bytes, 0);
    }
    return *this;
}

IndexPrefetchBudgetSlot::~IndexPrefetchBudgetSlot()
{
    reset();
}

IndexPrefetchBudgetSlot IndexPrefetchBudgetSlot::tryAcquire(IndexPrefetchBudget & budget, size_t bytes)
{
    IndexPrefetchBudgetSlot slot;
    if (!budget.tryAcquire(bytes))
    {
        ProfileEvents::increment(ProfileEvents::SkipIndexPrefetchBudgetExhausted);
        return slot;
    }

    slot.budget = &budget;
    slot.bytes = bytes;
    return slot;
}

void IndexPrefetchBudgetSlot::reset()
{
    if (budget)
    {
        budget->release(bytes);
        budget = nullptr;
        bytes = 0;
    }
}

bool canPrefetchIndexes(const IMergeTreeDataPartInfoForReader & part_info, const MergeTreeReaderSettings & settings)
{
    const auto & read_settings = settings.read_settings;
    if (!settings.index_prefetch_budget || read_settings.reader_executor.enabled)
        return false;

    if (part_info.getDataPartStorage()->isStoredOnRemoteDisk())
        return read_settings.remote_fs_settings.method == RemoteFSReadMethod::threadpool && read_settings.remote_fs_settings.prefetch;

    return read_settings.local_fs_settings.method == LocalFSReadMethod::pread_threadpool && read_settings.local_fs_settings.prefetch;
}

size_t estimatePrefetchBytes(
    const IMergeTreeDataPartInfoForReader & part_info, const MergeTreeReaderSettings & settings, std::optional<size_t> buffer_size)
{
    const auto & read_settings = settings.read_settings;
    const size_t max_buffer_size = part_info.getDataPartStorage()->isStoredOnRemoteDisk()
        ? read_settings.remote_fs_settings.buffer_size
        : read_settings.local_fs_settings.buffer_size;

    return 2 * std::min(buffer_size.value_or(max_buffer_size), max_buffer_size);
}

std::unique_ptr<PrefetchedSkipIndexReader> PrefetchedSkipIndexReader::tryCreate(
    const MergeTreeIndexPtr & index,
    MergeTreeIndexConditionPtr condition_,
    const MergeTreeDataPartInfoForReaderPtr & part_info,
    const MarkRanges & ranges,
    const MergeTreeReaderSettings & settings,
    MarkCache * mark_cache,
    UncompressedCache * uncompressed_cache,
    ThreadPool & pool)
{
    chassert(settings.index_prefetch_budget && !ranges.empty() && !index->isVectorSimilarityIndex());

    auto slot = IndexPrefetchBudgetSlot::tryAcquire(*settings.index_prefetch_budget, estimatePrefetchBytes(*part_info, settings));
    if (!slot)
        return nullptr;

    /// The same index ranges as in `filterMarksUsingIndex`.
    const size_t granularity = index->index.granularity;
    MarkRanges index_ranges;
    for (const auto & range : ranges)
        index_ranges.emplace_back(range.begin / granularity, (range.end + granularity - 1) / granularity);

    auto reader = std::make_unique<MergeTreeIndexReader>(
        index,
        part_info,
        part_info->getIndexGranularity().getMarksCountForSkipIndex(granularity),
        index_ranges,
        mark_cache,
        uncompressed_cache,
        /*vector_similarity_index_cache=*/ nullptr,
        settings,
        /*interruptible_marks_read=*/ true);

    const Priority priority = settings.read_settings.priority;
    auto res = std::make_unique<PrefetchedSkipIndexReader>(
        std::move(slot), std::move(reader), std::move(condition_), index_ranges.front().begin, pool, priority);

    res->task = res->runner.tryEnqueueAndKeepTrack([prefetched = res.get()] { prefetched->runJob(); }, priority);
    if (!res->task)
        return nullptr;

    ProfileEvents::increment(ProfileEvents::SkipIndexPrefetches);
    return res;
}

PrefetchedSkipIndexReader::PrefetchedSkipIndexReader(
    IndexPrefetchBudgetSlot slot_,
    std::unique_ptr<MergeTreeIndexReader> reader_,
    MergeTreeIndexConditionPtr condition_,
    size_t from_mark_,
    ThreadPool & pool,
    Priority priority_)
    : slot(std::move(slot_))
    , reader(std::move(reader_))
    , condition(std::move(condition_))
    , from_mark(from_mark_)
    , priority(priority_)
    , component(Coordination::getCurrentComponent())
    , runner(pool, ThreadName::MERGETREE_INDEX)
{
}

PrefetchedSkipIndexReader::~PrefetchedSkipIndexReader() = default;

void PrefetchedSkipIndexReader::runJob()
{
    /// The stream initialization waits for the marks, which may be read from a Keeper-backed disk.
    auto component_guard = Coordination::setCurrentComponent(component);

    fiu_do_on(FailPoints::skip_index_prefetch_job_exception,
    {
        throw Exception(ErrorCodes::FAULT_INJECTED, "Failpoint skip_index_prefetch_job_exception is enabled");
    });

    reader->prefetchBeginOfRange(from_mark, condition.get(), priority);
}

MergeTreeIndexReader & PrefetchedSkipIndexReader::take()
{
    if (auto claimed_task = std::exchange(task, nullptr))
    {
        if (Runner::tryCancel(*claimed_task))
        {
            ProfileEvents::increment(ProfileEvents::SkipIndexPrefetchesRunInline);
            runJob();
        }
        else
        {
            ProfileEventTimeIncrement<Microseconds> watch(ProfileEvents::SkipIndexPrefetchWaitMicroseconds);
            claimed_task->future.get();
        }
    }

    return *reader;
}

}
