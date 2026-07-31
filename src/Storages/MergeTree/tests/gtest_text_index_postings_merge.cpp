#include <gtest/gtest.h>

#include <Storages/MergeTree/BitpackingBlockCodec.h>
#include <Storages/MergeTree/MergeTreeIndexTextPostingListCodec.h>
#include <Storages/MergeTree/MergedPartOffsets.h>
#include <Storages/MergeTree/TextIndexPostingsMerge.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>

#include <algorithm>
#include <deque>
#include <optional>
#include <random>

using namespace DB;

namespace DB::ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

namespace
{

/// In-memory implementation of the postings stream seam used by the merge cursor.
class MemoryPostingsStream final : public IPostingsReadStream
{
public:
    explicit MemoryPostingsStream(std::string data_) : data(std::move(data_)) {}

    ReadBuffer & seekTo(UInt64 offset) override
    {
        buffer.emplace(data.data() + offset, data.size() - offset);
        return *buffer;
    }

private:
    std::string data;
    std::optional<ReadBufferFromMemory> buffer;
};

/// One encoded source posting list: the .pst byte stream plus its dictionary info.
struct EncodedSource
{
    std::string data;
    TokenPostingsInfo info;
    IPostingListCodec::Type codec_type = IPostingListCodec::Type::Bitpacking;
    MergeTreeIndexVersion version = static_cast<MergeTreeIndexVersion>(TextIndexHeader::Version::WithCodec);
};

EncodedSource encodeBitpackedSource(const std::vector<UInt32> & values, size_t posting_list_block_size)
{
    using enum PostingsSerialization::Flags;

    EncodedSource source;
    PostingListCodecBitpackingImpl impl(posting_list_block_size);
    for (UInt32 value : values)
        impl.insert(value);

    WriteBufferFromOwnString out;
    impl.encode(out, source.info);
    source.data = out.str();

    source.info.header = IsCompressed | HasBlockIndex;
    if (values.size() <= posting_list_block_size)
        source.info.header |= SingleBlock;
    source.info.cardinality = static_cast<UInt32>(values.size());
    return source;
}

/// Codec None source: one serialized roaring bitmap per posting list block.
/// Block cardinality is arbitrary, so cursor batches are misaligned with BLOCK_SIZE.
EncodedSource encodeRoaringSource(const std::vector<UInt32> & values, size_t block_cardinality)
{
    using enum PostingsSerialization::Flags;

    EncodedSource source;
    WriteBufferFromOwnString out;

    for (size_t begin = 0; begin < values.size(); begin += block_cardinality)
    {
        size_t end = std::min(values.size(), begin + block_cardinality);
        PostingList block(end - begin, values.data() + begin);
        block.runOptimize();

        source.info.offsets.push_back(out.count());
        source.info.ranges.emplace_back(values[begin], values[end - 1]);

        size_t num_bytes = block.getSizeInBytes(true);
        std::vector<char> bytes(num_bytes);
        block.write(bytes.data(), true);
        writeVarUInt(num_bytes, out);
        out.write(bytes.data(), num_bytes);
    }

    source.data = out.str();
    source.info.header = source.info.offsets.size() == 1 ? SingleBlock : 0;
    source.info.cardinality = static_cast<UInt32>(values.size());
    source.codec_type = IPostingListCodec::Type::None;
    return source;
}

EncodedSource encodeRawSource(const std::vector<UInt32> & values)
{
    using enum PostingsSerialization::Flags;

    EncodedSource source;
    WriteBufferFromOwnString out;
    source.info.offsets.push_back(out.count());
    source.info.ranges.emplace_back(values.front(), values.back());

    for (UInt32 value : values)
        writeVarUInt(value, out);

    source.data = out.str();
    source.info.header = RawPostings | SingleBlock;
    source.info.cardinality = static_cast<UInt32>(values.size());
    return source;
}

EncodedSource encodeEmbeddedSource(const std::vector<UInt32> & values)
{
    using enum PostingsSerialization::Flags;

    EncodedSource source;
    source.info.header = RawPostings | EmbeddedPostings;
    source.info.cardinality = static_cast<UInt32>(values.size());
    source.info.embedded_postings = std::make_shared<PostingList>(values.size(), values.data());
    return source;
}

/// Rewrites a bitpacked stream keeping only header + payload of each segment,
/// dropping the V2 index sections, as written by pre-HasBlockIndex versions.
EncodedSource stripIndexSections(const EncodedSource & source)
{
    EncodedSource stripped = source;
    stripped.info.offsets.clear();
    WriteBufferFromOwnString out;

    for (UInt64 offset : source.info.offsets)
    {
        ReadBufferFromMemory in(source.data.data() + offset, source.data.size() - offset);
        auto header = PostingListCodecBitpackingImpl::readSegmentHeader(in);
        size_t header_bytes = in.count();

        stripped.info.offsets.push_back(out.count());
        out.write(source.data.data() + offset, header_bytes + header.payload_bytes);
    }

    stripped.data = out.str();
    return stripped;
}

/// Random interleaving of `part_rows` parts into one merged sequence.
std::shared_ptr<MergedPartOffsets> makeInterleavedMapping(const std::vector<size_t> & part_rows, std::mt19937 & rng)
{
    std::vector<UInt64> part_sequence;
    for (size_t part = 0; part < part_rows.size(); ++part)
        part_sequence.insert(part_sequence.end(), part_rows[part], part);
    std::shuffle(part_sequence.begin(), part_sequence.end(), rng);

    auto mapping = std::make_shared<MergedPartOffsets>(part_rows.size());
    mapping->insert(part_sequence.data(), part_sequence.data() + part_sequence.size());
    mapping->flush();
    return mapping;
}

std::vector<UInt32> sampleRows(size_t num_rows, double probability, std::mt19937 & rng)
{
    std::vector<UInt32> rows;
    std::uniform_real_distribution<double> dist(0.0, 1.0);

    for (size_t row = 0; row < num_rows; ++row)
        if (dist(rng) < probability)
            rows.push_back(static_cast<UInt32>(row));

    if (rows.empty())
        rows.push_back(static_cast<UInt32>(rng() % num_rows));
    return rows;
}

/// Builds cursors for the sources and runs the k-way merge with the given consumer.
void runMergeWithConsumer(
    const std::vector<EncodedSource> & sources,
    const MergedPartOffsets * mapping,
    const std::vector<size_t> & part_indexes,
    const std::function<void(std::span<UInt32>)> & consume)
{
    std::deque<MemoryPostingsStream> streams;
    std::vector<std::unique_ptr<TokenPostingsMergeCursor>> holders;
    std::vector<TokenPostingsMergeCursor *> cursors;

    for (size_t i = 0; i < sources.size(); ++i)
    {
        streams.emplace_back(sources[i].data);
        holders.push_back(std::make_unique<TokenPostingsMergeCursor>(
            streams.back(), sources[i].info, sources[i].codec_type, sources[i].version, mapping, part_indexes[i]));
        cursors.push_back(holders.back().get());
    }

    mergeTokenPostings(cursors, consume);
}

/// Runs the merge and returns the merged values, checking staged block alignment.
std::vector<UInt32> runMerge(
    const std::vector<EncodedSource> & sources,
    const MergedPartOffsets * mapping,
    const std::vector<size_t> & part_indexes)
{
    std::vector<UInt32> result;
    bool saw_partial_block = false;

    runMergeWithConsumer(sources, mapping, part_indexes, [&](std::span<UInt32> block)
    {
        EXPECT_FALSE(saw_partial_block) << "only the final staged block may be partial";
        if (block.size() != BLOCK_SIZE)
            saw_partial_block = true;
        result.insert(result.end(), block.begin(), block.end());
    });

    return result;
}

/// Runs the merge into the streaming bitpacking sink, as the merge task does for large tokens.
std::pair<std::string, TokenPostingsInfo> encodeViaStreamingSink(
    const std::vector<EncodedSource> & sources,
    const MergedPartOffsets * mapping,
    const std::vector<size_t> & part_indexes,
    size_t posting_list_block_size)
{
    PostingListCodecBitpackingImpl impl(posting_list_block_size);

    runMergeWithConsumer(sources, mapping, part_indexes, [&](std::span<UInt32> block)
    {
        if (block.size() == BLOCK_SIZE)
            impl.insert(block);
        else
            for (UInt32 value : block)
                impl.insert(value);
    });

    TokenPostingsInfo info;
    WriteBufferFromOwnString out;
    impl.encode(out, info);
    return {out.str(), info};
}

/// Reference: the old merge path fed the sorted union of remapped values to the codec.
std::pair<std::string, TokenPostingsInfo> encodeViaOldPath(const std::vector<UInt32> & sorted_union, size_t posting_list_block_size)
{
    PostingList postings(sorted_union.size(), sorted_union.data());
    PostingListCodecBitpacking codec;

    TokenPostingsInfo info;
    WriteBufferFromOwnString out;
    codec.encode(postings, posting_list_block_size, info, out);
    return {out.str(), info};
}

std::vector<UInt32> remapAndSort(
    const std::vector<std::vector<UInt32>> & source_values,
    const MergedPartOffsets * mapping,
    const std::vector<size_t> & part_indexes)
{
    std::vector<UInt32> expected;
    for (size_t i = 0; i < source_values.size(); ++i)
    {
        for (UInt32 row : source_values[i])
        {
            UInt64 mapped = mapping ? (*mapping)[part_indexes[i], row] : row;
            expected.push_back(static_cast<UInt32>(mapped));
        }
    }
    std::sort(expected.begin(), expected.end());
    return expected;
}

}

TEST(TextIndexPostingsMerge, SingleCursorMultiSegment)
{
    std::mt19937 rng(42);

    /// Multi-segment bitpacked source with a partial tail, no remapping.
    auto values = sampleRows(5000, 0.6, rng);
    auto source = encodeBitpackedSource(values, 256);
    ASSERT_GT(source.info.offsets.size(), 1u);

    auto merged = runMerge({source}, nullptr, {0});
    EXPECT_EQ(merged, values);
}

TEST(TextIndexPostingsMerge, SingleCursorWithMapping)
{
    std::mt19937 rng(43);

    std::vector<size_t> part_rows = {4000};
    auto mapping = makeInterleavedMapping(part_rows, rng);

    auto values = sampleRows(part_rows[0], 0.3, rng);
    auto source = encodeBitpackedSource(values, 512);

    auto merged = runMerge({source}, mapping.get(), {0});
    EXPECT_EQ(merged, remapAndSort({values}, mapping.get(), {0}));
}

TEST(TextIndexPostingsMerge, EmbeddedAndRawSources)
{
    std::mt19937 rng(44);

    std::vector<size_t> part_rows = {100, 100, 100};
    auto mapping = makeInterleavedMapping(part_rows, rng);

    /// Totals straddle the embedded (6) and raw (12) thresholds.
    std::vector<std::vector<UInt32>> source_values = {{1, 5, 90}, {0, 42, 43}, {7, 8, 9, 10, 50, 60, 70}};
    std::vector<EncodedSource> sources;
    sources.push_back(encodeEmbeddedSource(source_values[0]));
    sources.push_back(encodeEmbeddedSource(source_values[1]));
    sources.push_back(encodeRawSource(source_values[2]));

    std::vector<size_t> part_indexes = {0, 1, 2};
    auto merged = runMerge(sources, mapping.get(), part_indexes);
    EXPECT_EQ(merged, remapAndSort(source_values, mapping.get(), part_indexes));
}

TEST(TextIndexPostingsMerge, MisalignedRoaringSourceBatches)
{
    std::mt19937 rng(45);

    std::vector<size_t> part_rows = {3000, 3000};
    auto mapping = makeInterleavedMapping(part_rows, rng);

    /// Roaring blocks of cardinality 100 produce batches misaligned with BLOCK_SIZE.
    std::vector<std::vector<UInt32>> source_values;
    source_values.push_back(sampleRows(part_rows[0], 0.4, rng));
    source_values.push_back(sampleRows(part_rows[1], 0.4, rng));

    std::vector<EncodedSource> sources;
    sources.push_back(encodeRoaringSource(source_values[0], 100));
    sources.push_back(encodeBitpackedSource(source_values[1], 256));

    std::vector<size_t> part_indexes = {0, 1};
    auto expected = remapAndSort(source_values, mapping.get(), part_indexes);
    auto merged = runMerge(sources, mapping.get(), part_indexes);
    EXPECT_EQ(merged, expected);

    /// The streaming sink must be byte-identical to the old union-then-encode path.
    auto [new_data, new_info] = encodeViaStreamingSink(sources, mapping.get(), part_indexes, 1024);
    auto [old_data, old_info] = encodeViaOldPath(expected, 1024);

    EXPECT_EQ(new_data, old_data);
    EXPECT_EQ(new_info.offsets, old_info.offsets);
    ASSERT_EQ(new_info.ranges.size(), old_info.ranges.size());
    for (size_t i = 0; i < new_info.ranges.size(); ++i)
    {
        EXPECT_EQ(new_info.ranges[i].begin, old_info.ranges[i].begin);
        EXPECT_EQ(new_info.ranges[i].end, old_info.ranges[i].end);
    }
}

TEST(TextIndexPostingsMerge, V1StyleSourceWithoutIndexSection)
{
    std::mt19937 rng(46);

    auto values = sampleRows(4000, 0.5, rng);
    auto source = encodeBitpackedSource(values, 256);
    ASSERT_GT(source.info.offsets.size(), 1u);

    /// Pre-WithCodec source: no per-segment index section, no persisted codec type
    /// (the header carries codec None) and no HasBlockIndex flag.
    auto stripped = stripIndexSections(source);
    stripped.info.header &= ~static_cast<UInt64>(PostingsSerialization::Flags::HasBlockIndex);
    stripped.codec_type = IPostingListCodec::Type::None;
    stripped.version = static_cast<MergeTreeIndexVersion>(TextIndexHeader::Version::Initial);

    auto merged = runMerge({stripped}, nullptr, {0});
    EXPECT_EQ(merged, values);
}

TEST(TextIndexPostingsMerge, CompressedNoneCodecSourceThrows)
{
    /// For WithCodec+ sources IsCompressed with codec None is corrupted data.
    auto source = encodeBitpackedSource({1, 2, 3, 100, 200}, 128);
    source.codec_type = IPostingListCodec::Type::None;

    MemoryPostingsStream stream(source.data);
    EXPECT_THROW(
        TokenPostingsMergeCursor(stream, source.info, source.codec_type, source.version, nullptr, 0),
        DB::Exception);
}

TEST(TextIndexPostingsMerge, OverlappingSourcesThrowLogicalError)
{
    /// Without remapping two sources sharing a row id violate strict monotonicity.
    std::vector<EncodedSource> sources;
    sources.push_back(encodeRawSource({1, 5, 9, 11, 12, 13, 14}));
    sources.push_back(encodeRawSource({5, 20, 21, 22, 23, 24, 25}));

    try
    {
        runMerge(sources, nullptr, {0, 1});
        FAIL() << "expected a LOGICAL_ERROR exception";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::LOGICAL_ERROR);
    }
}

TEST(TextIndexPostingsMerge, RandomizedKWayVsSortedUnion)
{
    std::mt19937 rng(20260731);

    for (int iteration = 0; iteration < 40; ++iteration)
    {
        size_t num_parts = 1 + rng() % 6;
        std::vector<size_t> part_rows(num_parts);
        for (auto & rows : part_rows)
            rows = 1 + rng() % 3000;

        auto mapping = makeInterleavedMapping(part_rows, rng);

        std::vector<std::vector<UInt32>> source_values;
        std::vector<EncodedSource> sources;
        std::vector<size_t> part_indexes;

        for (size_t part = 0; part < num_parts; ++part)
        {
            double probability = std::uniform_real_distribution<double>(0.005, 0.9)(rng);
            auto rows = sampleRows(part_rows[part], probability, rng);

            switch (rng() % 4)
            {
                case 0:
                {
                    static constexpr size_t segment_sizes[] = {128, 256, 384, 1024};
                    sources.push_back(encodeBitpackedSource(rows, segment_sizes[rng() % 4]));
                    break;
                }
                case 1:
                {
                    static constexpr size_t block_cardinalities[] = {5, 100, 1000};
                    sources.push_back(encodeRoaringSource(rows, block_cardinalities[rng() % 3]));
                    break;
                }
                case 2:
                {
                    if (rows.size() <= MAX_CARDINALITY_FOR_RAW_POSTINGS)
                        sources.push_back(encodeRawSource(rows));
                    else
                        sources.push_back(encodeBitpackedSource(rows, 256));
                    break;
                }
                case 3:
                {
                    if (rows.size() <= MAX_CARDINALITY_FOR_EMBEDDED_POSTINGS)
                        sources.push_back(encodeEmbeddedSource(rows));
                    else
                        sources.push_back(encodeRoaringSource(rows, 128));
                    break;
                }
            }

            source_values.push_back(std::move(rows));
            part_indexes.push_back(part);
        }

        auto expected = remapAndSort(source_values, mapping.get(), part_indexes);
        auto merged = runMerge(sources, mapping.get(), part_indexes);
        ASSERT_EQ(merged, expected) << "iteration " << iteration;

        /// Byte-identity of the streaming sink against the old union-then-encode path.
        if (expected.size() > MAX_CARDINALITY_FOR_RAW_POSTINGS)
        {
            static constexpr size_t destination_block_sizes[] = {256, 1024, 1024 * 1024};
            size_t destination_block_size = destination_block_sizes[rng() % 3];

            auto [new_data, new_info] = encodeViaStreamingSink(sources, mapping.get(), part_indexes, destination_block_size);
            auto [old_data, old_info] = encodeViaOldPath(expected, destination_block_size);

            ASSERT_EQ(new_data, old_data) << "iteration " << iteration;
            ASSERT_EQ(new_info.offsets, old_info.offsets) << "iteration " << iteration;
        }
    }
}
