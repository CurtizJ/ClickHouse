#pragma once

#include <Common/Priority.h>
#include <Common/StaticString.h>
#include <Common/ThreadPool_fwd.h>
#include <Common/threadPoolCallbackRunner.h>
#include <Storages/MergeTree/IMergeTreeDataPartInfoForReader.h>
#include <Storages/MergeTree/MergeTreeIndices.h>
#include <base/types.h>

#include <atomic>
#include <memory>
#include <optional>

namespace DB
{

class MarkCache;
class MergeTreeIndexReader;
class UncompressedCache;
struct MergeTreeReaderSettings;

/// Query-scoped caps on the skip index prefetches in flight: their number (`filesystem_prefetches_limit`,
/// 0 is unlimited, as in the prefetched read pool) and their estimated memory (`filesystem_prefetch_max_memory_usage`).
/// Created once per read step and shared by all copies of its `MergeTreeReaderSettings`.
class IndexPrefetchBudget
{
public:
    IndexPrefetchBudget(size_t max_prefetches_, size_t max_bytes_);

    /// Reserves a prefetch of `bytes`. Returns false and reserves nothing when a cap would be exceeded.
    bool tryAcquire(size_t bytes);
    void release(size_t bytes);

    /// 0 means unlimited.
    size_t getMaxPrefetches() const { return max_prefetches; }

private:
    const size_t max_prefetches;
    const size_t max_bytes;
    std::atomic<size_t> num_prefetches = 0;
    std::atomic<size_t> num_bytes = 0;
};

using IndexPrefetchBudgetPtr = std::shared_ptr<IndexPrefetchBudget>;

/// A reservation in `IndexPrefetchBudget`, released on destruction. Empty when the reservation failed.
class IndexPrefetchBudgetSlot
{
public:
    IndexPrefetchBudgetSlot() = default;
    IndexPrefetchBudgetSlot(IndexPrefetchBudgetSlot && other) noexcept;
    IndexPrefetchBudgetSlot & operator=(IndexPrefetchBudgetSlot && other) noexcept;
    ~IndexPrefetchBudgetSlot();

    /// Counts `SkipIndexPrefetchBudgetExhausted` when the budget has no room.
    static IndexPrefetchBudgetSlot tryAcquire(IndexPrefetchBudget & budget, size_t bytes);

    explicit operator bool() const { return budget != nullptr; }
    void reset();

private:
    IndexPrefetchBudget * budget = nullptr;
    size_t bytes = 0;
};

/// Whether skip index data of the part may be prefetched: `use_skip_indexes_prefetch` is on (the budget exists),
/// the reader executor is off (its `prefetch` is a no-op), and the part is read with an asynchronous method
/// with prefetch: `threadpool` for a remote disk, `pread_threadpool` for a local one.
bool canPrefetchIndexes(const IMergeTreeDataPartInfoForReader & part_info, const MergeTreeReaderSettings & settings);

/// Estimated memory of a prefetched stream of the part: its working buffer and its prefetch buffer.
/// Without `buffer_size`, the stream may use the whole buffer of the part's disk.
size_t estimatePrefetchBytes(
    const IMergeTreeDataPartInfoForReader & part_info, const MergeTreeReaderSettings & settings, std::optional<size_t> buffer_size = {});

/// A skip index reader whose first index mark is prefetched by a job on the prefetch pool ahead of the analysis.
/// `take` claims the job: one that has not started is cancelled and run inline (so a consumer that itself runs on
/// the prefetch pool cannot deadlock), otherwise it waits for the job and rethrows its error.
/// The destructor cancels a job that has not started or waits for a running one.
class PrefetchedSkipIndexReader
{
public:
    /// Creates the reader that `filterMarksUsingIndex` would create for `ranges` and schedules the prefetch.
    /// Returns nullptr when the budget is exhausted or the pool does not accept the job.
    static std::unique_ptr<PrefetchedSkipIndexReader> tryCreate(
        const MergeTreeIndexPtr & index,
        MergeTreeIndexConditionPtr condition_,
        const MergeTreeDataPartInfoForReaderPtr & part_info,
        const MarkRanges & ranges,
        const MergeTreeReaderSettings & settings,
        MarkCache * mark_cache,
        UncompressedCache * uncompressed_cache,
        ThreadPool & pool);

    PrefetchedSkipIndexReader(
        IndexPrefetchBudgetSlot slot_,
        std::unique_ptr<MergeTreeIndexReader> reader_,
        MergeTreeIndexConditionPtr condition_,
        size_t from_mark_,
        ThreadPool & pool,
        Priority priority_);

    ~PrefetchedSkipIndexReader();

    MergeTreeIndexReader & take();

private:
    using Runner = ThreadPoolCallbackRunnerLocal<void>;

    void runJob();

    /// Declared in the order of destruction dependencies: the runner waits for the job, which uses the reader.
    IndexPrefetchBudgetSlot slot;
    std::unique_ptr<MergeTreeIndexReader> reader;
    const MergeTreeIndexConditionPtr condition;
    const size_t from_mark;
    const Priority priority;
    const StaticString component;
    Runner runner;
    std::shared_ptr<Runner::Task> task;
};

}
