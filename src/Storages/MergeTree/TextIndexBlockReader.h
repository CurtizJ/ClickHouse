#pragma once

#include <Common/ProfileEvents.h>
#include <IO/ByteRangeSet.h>
#include <Storages/MergeTree/MergeTreeIOSettings.h>
#include <Storages/MergeTree/MergeTreeIndexPrefetch.h>
#include <Storages/MergeTree/MergeTreeIndicesSerialization.h>

#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace DB
{

class IMergeTreeDataPartInfoForReader;
struct IMergeTreeIndex;
class ReadBuffer;

/// Reads blocks of one text index substream by their offsets. Blocks announced by `enqueue`, in the order the analysis
/// reads them, are prefetched by `startPrefetches`. Blocks share a stream, which reads them with one request, while
/// that stream would serve them without a new request: in any order within its first buffer, and beyond it going
/// forward by less than the gap a remote read bridges. So prefetching needs no more requests than one stream without it.
/// At most `window` streams are in flight, each with a budget slot. Other blocks are read from one fallback stream.
class TextIndexBlockReader
{
public:
    /// The end of a block that ends at the end of the file.
    static constexpr UInt64 END_OF_FILE = std::numeric_limits<UInt64>::max();

    TextIndexBlockReader(
        const IMergeTreeDataPartInfoForReader & part_info_,
        const IMergeTreeIndex & index_,
        MergeTreeIndexSubstream::Type substream_type_,
        const MergeTreeReaderSettings & settings_,
        bool enable_prefetch);

    ~TextIndexBlockReader();

    bool isPrefetchEnabled() const { return budget != nullptr; }

    /// Announces the block at `offset`. `end` is its exact end if known, otherwise it ends about `expected_bytes` later.
    /// A no-op without prefetching and for a block that is already announced.
    void enqueue(UInt64 offset, std::optional<UInt64> end, size_t expected_bytes);

    /// Starts prefetching the announced blocks while the window and the budget allow.
    void startPrefetches();

    /// Returns the buffer positioned at the block, valid until the next call.
    /// A block that is not prefetched is read from the fallback stream with a buffer of `fallback_buffer_size`.
    ReadBuffer & readAt(UInt64 offset, size_t fallback_buffer_size);

    /// The announced block is not going to be read.
    void skip(UInt64 offset);

    /// None of the announced blocks are going to be read.
    void clear();

private:
    struct Group
    {
        UInt64 begin = 0;
        UInt64 end = 0;
        /// The end, or its estimate if the end is not exact or is the end of the file.
        UInt64 estimated_end = 0;
        /// The largest offset of a block. A group beyond its first buffer grows only past it.
        UInt64 max_block = 0;
        bool beyond_first_buffer = false;
        bool exact_end = false;
        std::vector<UInt64> blocks;
        size_t remaining_blocks = 0;
        ByteRangeSet ranges;
        IndexPrefetchBudgetSlot slot;
        /// Null until the prefetch starts.
        std::unique_ptr<MergeTreeReaderStream> stream;
    };

    using Groups = std::list<Group>;

    std::unique_ptr<MergeTreeReaderStream> makeStream(size_t buffer_size) const;
    bool canExtend(const Group & group, UInt64 offset, UInt64 estimated_end) const;
    void startGroup(Group & group, IndexPrefetchBudgetSlot slot);
    void eraseGroup(Groups::iterator group);
    void releaseCurrent();

    static constexpr size_t window = 8;

    const IMergeTreeDataPartInfoForReader & part_info;
    const IMergeTreeIndex & index;
    const MergeTreeIndexSubstream substream;
    const MergeTreeReaderSettings settings;
    IndexPrefetchBudget * const budget;
    const ProfileEvents::Event prefetched_event;
    /// Blocks at most this far apart are read with one request.
    const size_t max_gap;
    /// The largest buffer of a stream, see `makeTextIndexInputStream`.
    const size_t max_buffer_size;

    Groups groups;
    std::unordered_map<UInt64, Groups::iterator> group_by_block;
    size_t num_started = 0;
    /// The group whose buffer was returned last: kept until the next call, since the caller reads from it.
    std::optional<Groups::iterator> current;

    std::unique_ptr<MergeTreeReaderStream> fallback_stream;
    size_t fallback_stream_buffer_size = 0;
};

/// The block readers of the dictionary and the postings of a text index granule. A reader of a prefetchable part
/// creates it, possibly issues prefetches ahead (see `issueTextIndexPrefetches`), and passes it to the analysis
/// through `MergeTreeIndexDeserializationState`.
struct TextIndexPrefetchHandle
{
    TextIndexPrefetchHandle(
        const IMergeTreeDataPartInfoForReader & part_info, const IMergeTreeIndex & index, const MergeTreeReaderSettings & settings, bool enable_prefetch);

    TextIndexBlockReader dictionary;
    TextIndexBlockReader postings;
};

}
