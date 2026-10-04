#include <Storages/MergeTree/TextIndexBlockReader.h>

#include <Formats/MarkInCompressedFile.h>
#include <IO/ReadBuffer.h>
#include <Storages/MergeTree/IMergeTreeDataPartInfoForReader.h>
#include <Storages/MergeTree/MergeTreeIndices.h>
#include <Storages/MergeTree/MergeTreeReaderStream.h>
#include <Storages/MergeTree/TextIndexUtils.h>
#include <base/arithmeticOverflow.h>

#include <algorithm>

namespace ProfileEvents
{
    extern const Event TextIndexPrefetchedDictionaryBlocks;
    extern const Event TextIndexPrefetchedPostings;
    extern const Event TextIndexUnusedPrefetches;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

namespace
{

MergeTreeIndexSubstream getTextIndexSubstream(const IMergeTreeIndex & index, MergeTreeIndexSubstream::Type type)
{
    for (const auto & substream : index.getSubstreams())
    {
        if (substream.type == type)
            return substream;
    }

    throw Exception(ErrorCodes::LOGICAL_ERROR, "Index {} has no substream of type {}", index.index.name, static_cast<int>(type));
}

}

TextIndexBlockReader::TextIndexBlockReader(
    const IMergeTreeDataPartInfoForReader & part_info_,
    const IMergeTreeIndex & index_,
    MergeTreeIndexSubstream::Type substream_type_,
    const MergeTreeReaderSettings & settings_,
    bool enable_prefetch)
    : part_info(part_info_)
    , index(index_)
    , substream(getTextIndexSubstream(index_, substream_type_))
    , settings(settings_)
    , budget(enable_prefetch ? settings_.index_prefetch_budget.get() : nullptr)
    , prefetched_event(substream_type_ == MergeTreeIndexSubstream::Type::TextIndexDictionary
        ? ProfileEvents::TextIndexPrefetchedDictionaryBlocks
        : ProfileEvents::TextIndexPrefetchedPostings)
    , max_gap(part_info_.getDataPartStorage()->isStoredOnRemoteDisk() ? settings_.read_settings.remote_fs_settings.min_bytes_for_seek : 16 * 1024)
    , max_buffer_size(std::max<size_t>(
        16 * 1024,
        part_info_.getDataPartStorage()->isStoredOnRemoteDisk()
            ? settings_.read_settings.remote_fs_settings.buffer_size
            : settings_.read_settings.local_fs_settings.buffer_size))
{
}

TextIndexBlockReader::~TextIndexBlockReader()
{
    clear();
}

std::unique_ptr<MergeTreeReaderStream> TextIndexBlockReader::makeStream(size_t buffer_size) const
{
    return makeTextIndexInputStream(part_info, index.getFileName(), substream, settings, buffer_size);
}

bool TextIndexBlockReader::canExtend(const Group & group, UInt64 offset, UInt64 estimated_end) const
{
    const UInt64 span = std::max(group.estimated_end, estimated_end) - std::min(group.begin, offset);
    if (!group.beyond_first_buffer && span <= max_buffer_size)
        return true;

    UInt64 max_offset = 0;
    return offset > group.max_block && !common::addOverflow<UInt64>(group.end, max_gap, max_offset) && offset <= max_offset;
}

void TextIndexBlockReader::enqueue(UInt64 offset, std::optional<UInt64> end, size_t expected_bytes)
{
    if (!budget || group_by_block.contains(offset))
        return;

    const UInt64 estimated_end = end && *end != END_OF_FILE ? *end : offset + std::max<size_t>(expected_bytes, 1);
    const UInt64 block_end = end.value_or(estimated_end);

    /// A started group does not change: its bounds are set.
    if (groups.empty() || groups.back().stream || !canExtend(groups.back(), offset, estimated_end))
    {
        groups.emplace_back();
        groups.back().begin = offset;
        groups.back().max_block = offset;
    }

    auto & group = groups.back();
    group.begin = std::min(group.begin, offset);
    group.max_block = std::max(group.max_block, offset);
    if (block_end >= group.end)
    {
        group.end = block_end;
        group.exact_end = end.has_value();
    }
    group.estimated_end = std::max(group.estimated_end, estimated_end);
    group.beyond_first_buffer = group.beyond_first_buffer || group.estimated_end - group.begin > max_buffer_size;
    group.blocks.push_back(offset);
    ++group.remaining_blocks;

    if (block_end != END_OF_FILE)
        group.ranges.add({offset, block_end - offset});

    group_by_block.emplace(offset, std::prev(groups.end()));
}

void TextIndexBlockReader::startPrefetches()
{
    if (!budget)
        return;

    for (auto it = groups.begin(); it != groups.end() && num_started < window; ++it)
    {
        if (it->stream)
            continue;

        auto slot = IndexPrefetchBudgetSlot::tryAcquire(*budget, estimatePrefetchBytes(part_info, settings, it->estimated_end - it->begin));
        if (!slot)
            return;

        startGroup(*it, std::move(slot));
    }
}

void TextIndexBlockReader::startGroup(Group & group, IndexPrefetchBudgetSlot slot)
{
    group.stream = makeStream(group.estimated_end - group.begin);
    auto * buffer = group.stream->getDataBuffer();

    if (group.end == END_OF_FILE)
    {
        group.end = group.stream->getFileSize();
        group.ranges.add({group.max_block, group.end - group.max_block});
    }

    /// The bounds are set before the first seek and never change, so the stream reads the group with one request.
    buffer->setRequestMap(group.ranges);
    if (group.exact_end)
        buffer->setReadUntilPosition(group.end);

    group.stream->seekToMark(MarkInCompressedFile{group.begin, 0});
    buffer->prefetch(settings.read_settings.priority);

    group.slot = std::move(slot);
    ++num_started;
    ProfileEvents::increment(prefetched_event, group.blocks.size());
}

void TextIndexBlockReader::eraseGroup(Groups::iterator group)
{
    if (group->stream)
    {
        --num_started;
        ProfileEvents::increment(ProfileEvents::TextIndexUnusedPrefetches, group->remaining_blocks);
    }

    for (UInt64 offset : group->blocks)
    {
        if (auto it = group_by_block.find(offset); it != group_by_block.end() && it->second == group)
            group_by_block.erase(it);
    }

    if (current == group)
        current.reset();

    groups.erase(group);
}

void TextIndexBlockReader::releaseCurrent()
{
    if (!current)
        return;

    auto group = *current;
    current.reset();

    if (group->remaining_blocks == 0)
    {
        eraseGroup(group);
        startPrefetches();
    }
}

ReadBuffer & TextIndexBlockReader::readAt(UInt64 offset, size_t fallback_buffer_size)
{
    releaseCurrent();

    if (auto it = group_by_block.find(offset); it != group_by_block.end())
    {
        auto group = it->second;
        group_by_block.erase(it);
        --group->remaining_blocks;

        if (group->stream)
        {
            group->stream->seekToMark(MarkInCompressedFile{offset, 0});
            auto * buffer = group->stream->getDataBuffer();

            /// Reads ahead the next blocks of the group, the stream is bounded by the group.
            if (group->exact_end)
                buffer->prefetch(settings.read_settings.priority);

            current = group;
            return *buffer;
        }

        /// The group is not started, so its blocks are read as without prefetching.
        eraseGroup(group);
    }

    if (!fallback_stream || fallback_stream_buffer_size < fallback_buffer_size)
    {
        fallback_stream = makeStream(fallback_buffer_size);
        fallback_stream_buffer_size = fallback_buffer_size;
    }

    fallback_stream->seekToMark(MarkInCompressedFile{offset, 0});
    return *fallback_stream->getDataBuffer();
}

void TextIndexBlockReader::skip(UInt64 offset)
{
    releaseCurrent();

    auto it = group_by_block.find(offset);
    if (it == group_by_block.end())
        return;

    auto group = it->second;
    group_by_block.erase(it);
    --group->remaining_blocks;

    if (group->stream)
        ProfileEvents::increment(ProfileEvents::TextIndexUnusedPrefetches);

    if (group->remaining_blocks == 0)
    {
        eraseGroup(group);
        startPrefetches();
    }
}

void TextIndexBlockReader::clear()
{
    current.reset();

    while (!groups.empty())
        eraseGroup(groups.begin());

    chassert(group_by_block.empty() && num_started == 0);
}

TextIndexPrefetchHandle::TextIndexPrefetchHandle(
    const IMergeTreeDataPartInfoForReader & part_info, const IMergeTreeIndex & index, const MergeTreeReaderSettings & settings, bool enable_prefetch)
    : dictionary(part_info, index, MergeTreeIndexSubstream::Type::TextIndexDictionary, settings, enable_prefetch)
    , postings(part_info, index, MergeTreeIndexSubstream::Type::TextIndexPostings, settings, enable_prefetch)
{
}

}
