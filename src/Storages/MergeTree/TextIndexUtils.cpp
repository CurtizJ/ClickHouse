#include <Processors/Port.h>
#include <DataTypes/DataTypeString.h>
#include <Storages/MergeTree/TextIndexUtils.h>
#include <Parsers/ExpressionElementParsers.h>
#include <Compression/CompressionFactory.h>
#include <Common/CurrentThread.h>
#include <Common/ProfileEvents.h>
#include <Common/ThreadStatus.h>
#include <Parsers/parseQuery.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Storages/MergeTree/MergeTreeData.h>
#include <Storages/MergeTree/MergeTreeIOSettings.h>
#include <Storages/MergeTree/MergeTreeIndexText.h>
#include <Storages/MergeTree/MergeTreeIndices.h>
#include <Storages/MergeTree/MergeTreeIndicesSerialization.h>
#include <Storages/MergeTree/MergeTreeIndexTextPostingListCodec.h>
#include <Storages/MergeTree/TextIndexPositionCodec.h>
#include <Storages/MergeTree/TextIndexPostingsMerge.h>
#include <Storages/MergeTree/MergeTreeReaderStream.h>
#include <Storages/MergeTree/MergeTreeSettings.h>
#include <Storages/MergeTree/ParallelSyncFiles.h>
#include <Disks/SingleDiskVolume.h>
#include <Storages/MergeTree/DataPartStorageOnDiskFull.h>
#include <Storages/MergeTree/MergeTreeIndexReader.h>

#include <deque>
#include <limits>

namespace ProfileEvents
{
    extern const Event TextIndexTemporarySegmentsWritten;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int FILE_DOESNT_EXIST;
    extern const int SUPPORT_IS_DISABLED;
}

namespace MergeTreeSetting
{
    extern const MergeTreeSettingsMilliseconds background_task_preferred_step_execution_time_ms;
    extern const MergeTreeSettingsNonZeroUInt64 text_index_max_memory_usage_before_flush;
    extern const MergeTreeSettingsNonZeroUInt64 text_index_max_processed_tokens_before_flush;
}

namespace
{

Int64 getCurrentThreadMemoryUsage()
{
    const auto & thread = CurrentThread::get();
    return thread.memory_tracker.get() + thread.untracked_memory.load();
}

CompressionCodecPtr makeMarksCompressionCodec(const String & marks_compression_codec)
{
    ParserCodec codec_parser;
    auto ast = parseQuery(codec_parser, "(" + Poco::toUpper(marks_compression_codec) + ")", 0, DBMS_DEFAULT_MAX_PARSER_DEPTH, DBMS_DEFAULT_MAX_PARSER_BACKTRACKS);
    return CompressionCodecFactory::instance().get(ast, nullptr);
}

std::pair<MergeTreeIndexOutputStreams, std::vector<std::unique_ptr<MergeTreeIndexWriterStream>>>
makeOutputStreams(
    const MergeTreeIndexSubstreams & index_substreams,
    const String & index_name,
    const MutableDataPartStoragePtr & data_part_storage,
    const CompressionCodecPtr & default_codec,
    const String & marks_file_extension,
    const MergeTreeWriterSettings & settings)
{
    auto marks_compression_codec = makeMarksCompressionCodec(settings.marks_compression_codec);
    MergeTreeIndexOutputStreams streams;
    std::vector<std::unique_ptr<MergeTreeIndexWriterStream>> streams_holders;

    for (const auto & index_substream : index_substreams)
    {
        auto stream_name = index_name + index_substream.suffix;

        auto stream = std::make_unique<MergeTreeIndexWriterStream>(
            stream_name,
            data_part_storage,
            stream_name,
            index_substream.extension,
            stream_name,
            marks_file_extension,
            default_codec,
            settings.max_compress_block_size,
            marks_compression_codec,
            settings.marks_compress_block_size,
            settings.query_write_settings);

        streams[index_substream.type] = stream.get();
        streams_holders.push_back(std::move(stream));
    }

    return {std::move(streams), std::move(streams_holders)};
}

void writeMarks(MergeTreeIndexOutputStreams & streams, bool can_use_adaptive_granularity)
{
    for (const auto & [_, stream] : streams)
    {
        auto & marks_out = stream->compress_marks ? stream->marks_compressed_hashing : stream->marks_hashing;

        writeBinaryLittleEndian(stream->plain_hashing.count(), marks_out);
        writeBinaryLittleEndian(stream->compressed_hashing.offset(), marks_out);
        if (can_use_adaptive_granularity)
            writeBinaryLittleEndian(1UL, marks_out);
    }
}

}

BuildTextIndexTransform::BuildTextIndexTransform(
    SharedHeader header,
    String index_file_prefix_,
    std::vector<MergeTreeIndexPtr> indexes_,
    MutableDataPartStoragePtr temporary_storage_,
    MergeTreeWriterSettings writer_settings_,
    CompressionCodecPtr default_codec_,
    String marks_file_extension_,
    const MergeTreeSettings & storage_settings)
    : ISimpleTransform(header, header, false)
    , index_file_prefix(std::move(index_file_prefix_))
    , indexes(std::move(indexes_))
    , temporary_storage(std::move(temporary_storage_))
    , writer_settings(std::move(writer_settings_))
    , default_codec(std::move(default_codec_))
    , marks_file_extension(std::move(marks_file_extension_))
    , segment_numbers(indexes.size(), 0)
    , estimated_allocated_bytes(indexes.size(), 0)
    , max_processed_tokens(storage_settings[MergeTreeSetting::text_index_max_processed_tokens_before_flush])
    , max_allocated_bytes(storage_settings[MergeTreeSetting::text_index_max_memory_usage_before_flush])
{

    for (size_t i = 0; i < indexes.size(); ++i)
    {
        auto aggregator = indexes[i]->createIndexAggregator();
        aggregators.push_back(std::move(aggregator));
        index_position_by_name.emplace(indexes[i]->index.name, i);
    }
}

void BuildTextIndexTransform::transform(Chunk & chunk)
{
    auto block = getInputPort().getHeader().cloneWithColumns(chunk.getColumns());
    aggregate(block);
}

IProcessor::Status BuildTextIndexTransform::prepare()
{
    auto status = ISimpleTransform::prepare();
    if (status == Status::Finished)
        finalize();
    return status;
}

void BuildTextIndexTransform::aggregate(const Block & block)
{
    if (block.rows() == 0)
        return;

    num_processed_rows += block.rows();

    for (size_t i = 0; i < indexes.size(); ++i)
    {
        size_t pos = 0;
        auto & aggregator_text = typeid_cast<MergeTreeIndexAggregatorText &>(*aggregators[i]);
        const auto memory_usage_before_update = getCurrentThreadMemoryUsage();
        aggregator_text.update(block, &pos, block.rows());
        const auto memory_usage_after_update = getCurrentThreadMemoryUsage();

        if (memory_usage_after_update > memory_usage_before_update)
            estimated_allocated_bytes[i] += static_cast<size_t>(memory_usage_after_update - memory_usage_before_update);

        if (aggregator_text.getNumProcessedTokens() > max_processed_tokens
            || estimated_allocated_bytes[i] > max_allocated_bytes)
            writeTemporarySegment(i);
    }
}

void BuildTextIndexTransform::finalize()
{
    for (size_t i = 0; i < indexes.size(); ++i)
    {
        if (!aggregators[i]->empty())
            writeTemporarySegment(i);
    }
}

std::vector<TextIndexSegment> BuildTextIndexTransform::getSegments(const String & index_name, size_t part_idx) const
{
    auto it = index_position_by_name.find(index_name);
    if (it == index_position_by_name.end())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Index {} not found in BuildTextIndexTransform", index_name);

    size_t index_idx = it->second;
    std::vector<TextIndexSegment> segments;

    for (size_t i = 0; i < segment_numbers[index_idx]; ++i)
    {
        auto index_file_name = fmt::format("{}_{}_{}", index_file_prefix, i, indexes[index_idx]->getFileName());
        segments.emplace_back(temporary_storage, std::move(index_file_name), part_idx);
    }

    return segments;
}

void BuildTextIndexTransform::writeTemporarySegment(size_t i)
{
    auto index_file_name = fmt::format("{}_{}_{}", index_file_prefix, segment_numbers[i]++, indexes[i]->getFileName());
    auto index_substreams = indexes[i]->getSubstreams();

    auto & aggregator_text = typeid_cast<MergeTreeIndexAggregatorText &>(*aggregators[i]);
    auto granule = aggregator_text.getGranuleAndReset();
    estimated_allocated_bytes[i] = 0;
    aggregator_text.setCurrentRow(num_processed_rows);

    auto [streams, streams_holders] = makeOutputStreams(
        index_substreams,
        index_file_name,
        temporary_storage,
        default_codec,
        marks_file_extension,
        writer_settings);

    writeMarks(streams, writer_settings.can_use_adaptive_granularity);
    granule->serializeBinaryWithMultipleStreams(streams);

    for (auto & stream : streams_holders)
        stream->finalize();

    ProfileEvents::increment(ProfileEvents::TextIndexTemporarySegmentsWritten);
}

static PostingsSerialization createPostingsSerialization(const IMergeTreeIndex & index)
{
    const auto * codec = typeid_cast<const MergeTreeIndexText &>(index).getPostingListCodec();
    auto codec_type = codec ? codec->getType() : IPostingListCodec::Type::None;
    auto codec_copy = PostingListCodecFactory::createPostingListCodec(codec_type);

    /// The merged part is written in the current on-disk format.
    return PostingsSerialization(std::move(codec_copy), static_cast<MergeTreeIndexVersion>(TextIndexHeader::Version::WithCodec));
}

static TextIndexHeader readSourceHeaderPrefix(MergeTreeIndexReaderStream & header_stream)
{
    header_stream.seekToStart();
    /// Only the version and codec are needed here, so skip deserializing the sparse index.
    return TextIndexSerialization::deserializeHeaderPrefix(*header_stream.getDataBuffer());
}

MergeTextIndexesTask::MergeTextIndexesTask(
    std::vector<TextIndexSegment> segments_,
    MergeTreeMutableDataPartPtr new_data_part_,
    size_t num_rows_,
    MergeTreeIndexPtr index_ptr_,
    std::shared_ptr<MergedPartOffsets> merged_part_offsets_,
    const MergeTreeReaderSettings & reader_settings_,
    const MergeTreeWriterSettings & writer_settings_,
    bool sync_)
    : segments(std::move(segments_))
    , new_data_part(std::move(new_data_part_))
    , num_rows(num_rows_)
    , index_ptr(std::move(index_ptr_))
    , merged_part_offsets(std::move(merged_part_offsets_))
    , writer_settings(writer_settings_)
    , sync(sync_)
    , step_time_ms((*new_data_part->storage.getSettings())[MergeTreeSetting::background_task_preferred_step_execution_time_ms].totalMilliseconds())
    , postings_serialization(createPostingsSerialization(*index_ptr))
{
    /// A mapping-disabled MergedPartOffsets cannot remap posting row ids;
    /// callers must pass null to request the identity mapping instead.
    if (merged_part_offsets && !merged_part_offsets->isMappingEnabled())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Text index merge task got merged part offsets with disabled mapping");

    cursors.resize(segments.size());
    inputs.resize(segments.size());
    input_streams.resize(segments.size());

    output_tokens = ColumnString::create();
    params = typeid_cast<const MergeTreeIndexText &>(*index_ptr).getParams();
    sparse_index_tokens = ColumnString::create();
    sparse_index_offsets = ColumnUInt64::create();

    std::tie(output_streams, output_streams_holders) = makeOutputStreams(
        index_ptr->getSubstreams(),
        index_ptr->getFileName(),
        new_data_part->getDataPartStoragePtr(),
        new_data_part->default_codec,
        new_data_part->getMarksFileExtension(),
        writer_settings);

    auto substreams = index_ptr->getSubstreams();

    for (size_t i = 0; i < segments.size(); ++i)
    {
        for (const auto & substream : substreams)
        {
            auto stream = makeTextIndexInputStream(
                segments[i].part_storage,
                segments[i].index_file_name + substream.suffix,
                substream.extension,
                MergeTreeIndexReader::patchSettings(reader_settings_, substream.type));

            input_streams[i][substream.type] = stream.get();
            input_streams_holders.emplace_back(std::move(stream));
        }
    }

    /// Resolve each source part's codec and serialization version from its own header.
    source_postings_serializations.reserve(segments.size());
    source_formats.reserve(segments.size());

    for (size_t i = 0; i < segments.size(); ++i)
    {
        auto * stream = input_streams[i].at(MergeTreeIndexSubstream::Type::Regular);
        auto header = readSourceHeaderPrefix(*stream);

        source_formats.push_back({header.codec_type, header.version});
        source_postings_serializations.emplace_back(PostingListCodecFactory::createPostingListCodec(header.codec_type), header.version);
    }
}

MergeTextIndexesTask::~MergeTextIndexesTask() noexcept
{
    cancelImpl();
}

Block MergeTextIndexesTask::getHeader() const
{
    return Block{ColumnWithTypeAndName{ColumnString::create(), std::make_shared<DataTypeString>(), "token"}};
}

void MergeTextIndexesTask::initializeQueue()
{
    SortDescription description;
    description.emplace_back("token");

    for (size_t source_num = 0; source_num < inputs.size(); ++source_num)
    {
        cursors[source_num] = SortCursorImpl(getHeader(), description, source_num);
        readDictionaryBlock(source_num);
    }
}

void MergeTextIndexesTask::readDictionaryBlock(size_t source_num)
{
    auto * stream = input_streams[source_num].at(MergeTreeIndexSubstream::Type::TextIndexDictionary);
    auto * data_buffer = stream->getDataBuffer();

    if (data_buffer->eof())
        return;

    inputs[source_num] = TextIndexSerialization::deserializeDictionaryBlock(*data_buffer, &source_postings_serializations[source_num]);
    const auto & tokens = inputs[source_num].tokens;
    cursors[source_num].reset({tokens}, getHeader(), tokens->size());
    queue.push(cursors[source_num]);
}

void MergeTextIndexesTask::flushPostingList()
{
    chassert(!pending_postings.empty());
    auto * postings_stream = output_streams.at(MergeTreeIndexSubstream::Type::TextIndexPostings);

    /// The offsets mapping is injective and each source contributes the token at most once,
    /// so the merged cardinality is the sum of the source cardinalities. This allows choosing
    /// the output format up front, before reading any postings.
    UInt64 total_cardinality = 0;
    for (const auto & pending : pending_postings)
        total_cardinality += pending.info.cardinality;

    std::deque<PostingsReaderStreamAdapter> stream_adapters;
    std::vector<std::unique_ptr<TokenPostingsMergeCursor>> cursor_holders;
    std::vector<TokenPostingsMergeCursor *> merge_cursors;
    cursor_holders.reserve(pending_postings.size());
    merge_cursors.reserve(pending_postings.size());

    for (const auto & pending : pending_postings)
    {
        auto * stream = input_streams[pending.source_num].at(MergeTreeIndexSubstream::Type::TextIndexPostings);
        stream_adapters.emplace_back(*stream);

        cursor_holders.push_back(std::make_unique<TokenPostingsMergeCursor>(
            stream_adapters.back(),
            pending.info,
            source_formats[pending.source_num].codec_type,
            source_formats[pending.source_num].version,
            merged_part_offsets.get(),
            segments[pending.source_num].part_index));

        merge_cursors.push_back(cursor_holders.back().get());
    }

    TokenPostingsInfo token_info;
    const auto * destination_codec = postings_serialization.getPostingListCodec();

    if (total_cardinality > MAX_CARDINALITY_FOR_RAW_POSTINGS && destination_codec->getType() == IPostingListCodec::Type::Bitpacking)
    {
        /// Stream the merged row ids directly into the bitpacking codec,
        /// without materializing an intermediate posting list.
        using enum PostingsSerialization::Flags;
        PostingListCodecBitpackingImpl codec_impl(params.posting_list_block_size);

        mergeTokenPostings(merge_cursors, [&](std::span<UInt32> block)
        {
            /// All staged blocks are full except the last one, keeping the codec block-aligned.
            if (block.size() == BLOCK_SIZE)
                codec_impl.insert(block);
            else
                for (UInt32 row_id : block)
                    codec_impl.insert(row_id);
        });

        token_info.header = IsCompressed | HasBlockIndex;
        if (total_cardinality <= params.posting_list_block_size)
            token_info.header |= SingleBlock;

        chassert(total_cardinality <= std::numeric_limits<UInt32>::max());
        token_info.cardinality = static_cast<UInt32>(total_cardinality);
        codec_impl.encode(postings_stream->plain_hashing, token_info);
    }
    else
    {
        /// Tiny posting lists and codec None destinations reuse the roaring serialization path.
        mergeTokenPostings(merge_cursors, [&](std::span<UInt32> block)
        {
            output_postings.addMany(block.size(), block.data());
        });

        PostingListBuilder builder(&output_postings);
        token_info = TextIndexSerialization::serializePostings(builder, *postings_stream, params, postings_serialization);

        if (token_info.header & PostingsSerialization::Flags::EmbeddedPostings)
            token_info.embedded_postings = std::make_shared<PostingList>(output_postings);
    }

    /// Serialize position data if positions are enabled.
    if (params.positions && !output_positions.empty())
    {
        auto * positions_stream = output_streams.at(MergeTreeIndexSubstream::Type::TextIndexPositions);

        /// Entries from multiple source parts may interleave after doc_id remapping.
        std::sort(output_positions.begin(), output_positions.end());

        size_t out = 0;
        for (size_t i = 1; i < output_positions.size(); ++i)
        {
            if (output_positions[out].sameBucket(output_positions[i]))
                output_positions[out].mergeBitmap(output_positions[i]);
            else
                output_positions[++out] = output_positions[i];
        }
        output_positions.resize(out + 1);

        token_info.header |= PostingsSerialization::Flags::HasPositions;
        token_info.position_offset = positions_stream->plain_hashing.count();
        token_info.position_cardinality = static_cast<UInt32>(output_positions.size());

        TextIndexPositionCodec::encode(output_positions, positions_stream->plain_hashing);
    }

    output_infos.push_back(token_info);
    output_postings.clear();
    output_positions.clear();
    pending_postings.clear();
}

void MergeTextIndexesTask::flushDictionaryBlock()
{
    if (output_tokens->size() != output_infos.size())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Tokens size ({}) doesn't match infos size ({})", output_tokens->size(), output_infos.size());

    if (output_infos.empty())
        return;

    auto tokens_format = params.dictionary_block_frontcoding_compression
        ? TextIndexSerialization::TokensFormat::FrontCodedStrings
        : TextIndexSerialization::TokensFormat::RawStrings;

    size_t num_tokens = output_infos.size();
    auto & output_str = assert_cast<ColumnString &>(*output_tokens);
    auto * dictionary_stream = output_streams.at(MergeTreeIndexSubstream::Type::TextIndexDictionary);
    auto & ostr = dictionary_stream->compressed_hashing;

    ostr.next();
    auto current_mark = dictionary_stream->getCurrentMark();
    chassert(current_mark.offset_in_decompressed_block == 0);

    auto first_token = output_tokens->getDataAt(0);
    assert_cast<ColumnString &>(*sparse_index_tokens).insertData(first_token.data(), first_token.size());
    assert_cast<ColumnUInt64 &>(*sparse_index_offsets).insertValue(current_mark.offset_in_compressed_file);

    TextIndexSerialization::serializeTokens(output_str, ostr, tokens_format);

    for (size_t i = 0; i < num_tokens; ++i)
    {
        TextIndexSerialization::serializeTokenInfo(ostr, output_infos[i]);

        if (output_infos[i].header & PostingsSerialization::Flags::EmbeddedPostings)
        {
            const auto & roaring_bitmap = output_infos[i].embedded_postings->roaring;
            postings_serialization.serialize(roaring_bitmap, output_infos[i].header, ostr);
        }
    }

    output_tokens = ColumnString::create();
    output_postings.clear();
    output_infos.clear();
}

bool MergeTextIndexesTask::isNewToken(const SortCursor & cursor) const
{
    const auto & input_str = assert_cast<const ColumnString &>(*inputs[cursor->order].tokens);
    const auto & output_str = assert_cast<const ColumnString &>(*output_tokens);

    return output_str.empty() || input_str.compareAt(cursor->getRow(), output_str.size() - 1, output_str, 1) != 0;
}

bool MergeTextIndexesTask::executeStep()
{
    if (!is_initialized)
    {
        is_initialized = true;
        initializeQueue();
        /// Write marks for compatibility with other skip indexes.
        /// An empty part carries no marks at all, exactly like every other skip index on an
        /// empty part. Writing one here would leave the marks file with a single mark while
        /// `getMarksCountForSkipIndex` reports zero, so reading the marks back (e.g. when the
        /// mark cache is prewarmed on attach) fails with `Too many marks in file`.
        /// The part is not finalized yet at this stage, so its `index_granularity` is empty;
        /// rely on the merged row count instead.
        chassert(new_data_part);
        if (num_rows != 0)
        {
            bool can_use_adaptive_granularity = new_data_part->index_granularity_info.mark_type.adaptive;
            writeMarks(output_streams, can_use_adaptive_granularity);
        }
    }

    if (!queue.isValid())
    {
        finalize();
        return false;
    }

    Stopwatch watch(CLOCK_MONOTONIC_COARSE);

    do
    {
        SortCursor current = queue.current();

        if (isNewToken(current))
        {
            if (!pending_postings.empty())
                flushPostingList();

            if (output_tokens->size() >= params.dictionary_block_size)
                flushDictionaryBlock();

            output_tokens->insertFrom(*inputs[current->order].tokens, current->getRow());
        }

        /// Postings are read and merged at flush time. The info must be copied because
        /// readDictionaryBlock below may replace the source block before the token is flushed.
        pending_postings.push_back({current->order, inputs[current->order].token_infos[current->getRow()]});

        /// Read and merge position data if positions are enabled.
        if (params.positions)
        {
            const auto & token_info = inputs[current->order].token_infos[current->getRow()];
            if (token_info.header & PostingsSerialization::Flags::HasPositions)
            {
                auto * pos_stream = input_streams[current->order].at(MergeTreeIndexSubstream::Type::TextIndexPositions);
                auto * pos_data_buffer = pos_stream->getDataBuffer();
                pos_stream->seekToMark({token_info.position_offset, 0});

                PODArray<RoaringishEntry> position_entries;
                TextIndexPositionCodec::decode(*pos_data_buffer, position_entries);

                /// Adjust doc_ids if merging parts with offset remapping.
                if (merged_part_offsets)
                {
                    size_t part_index = segments[current->order].part_index;
                    for (auto & entry : position_entries)
                    {
                        UInt64 new_doc_id = (*merged_part_offsets)[part_index, entry.doc_id];
                        if (new_doc_id > std::numeric_limits<UInt32>::max())
                            throw Exception(ErrorCodes::SUPPORT_IS_DISABLED,
                                "Cannot merge text index: remapped row id {} exceeds the maximum supported row id {}",
                                new_doc_id, std::numeric_limits<UInt32>::max());
                        entry = entry.withDocId(static_cast<UInt32>(new_doc_id));
                    }
                }

                output_positions.insert(output_positions.end(), position_entries.begin(), position_entries.end());
            }
        }

        if (!current->isLast())
        {
            queue.next();
        }
        else
        {
            queue.removeTop();
            readDictionaryBlock(current->order);
        }
    } while (queue.isValid() && watch.elapsedMilliseconds() < step_time_ms);

    return true;
}

void MergeTextIndexesTask::finalize()
{
    if (!pending_postings.empty())
        flushPostingList();

    if (!output_tokens->empty())
        flushDictionaryBlock();

    auto * index_stream = output_streams.at(MergeTreeIndexSubstream::Type::Regular);
    DictionarySparseIndex sparse_index(std::move(sparse_index_tokens), std::move(sparse_index_offsets));

    auto serialization_version = static_cast<MergeTreeIndexVersion>(
        params.positions ? TextIndexHeader::Version::WithPositions : TextIndexHeader::Version::WithCodec);
    TextIndexSerialization::serializeHeader(sparse_index, postings_serialization.getPostingListCodec()->getType(), serialization_version, params.positions, index_stream->compressed_hashing);

    for (auto & stream : output_streams_holders)
        stream->finalize();

    /// fsync the index files, like `MergeTreeDataPartWriterOnDisk::finishSkipIndicesSerialization` does.
    if (sync)
    {
        std::vector<const MergeTreeWriterStream *> streams_to_sync;
        streams_to_sync.reserve(output_streams_holders.size());
        for (const auto & stream : output_streams_holders)
            streams_to_sync.push_back(stream.get());
        parallelSyncFiles(streams_to_sync);
    }
}

void MergeTextIndexesTask::cancel() noexcept
{
    cancelImpl();
}

void MergeTextIndexesTask::cancelImpl() noexcept
{
    try
    {
        for (auto & stream : output_streams_holders)
            stream->cancel();
    }
    catch (...)
    {
        tryLogCurrentException(__PRETTY_FUNCTION__);
    }
}

void MergeTextIndexesTask::addToChecksums(MergeTreeDataPartChecksums & checksums)
{
    for (const auto & [type, stream] : output_streams)
        stream->addToChecksums(checksums, MergeTreeIndexSubstream::isCompressed(type));
}

MutableDataPartStoragePtr createTemporaryTextIndexStorage(const DiskPtr & disk, const String & part_relative_path)
{
    static constexpr const char * temp_part_dir = "text_index_tmp";
    auto volume = std::make_shared<SingleDiskVolume>("volume_" + part_relative_path + "_" + temp_part_dir, disk, 0);
    auto storage = std::make_shared<DataPartStorageOnDiskFull>(volume, part_relative_path, temp_part_dir);
    storage->beginTransaction();
    storage->createDirectories();
    return storage;
}

std::unique_ptr<MergeTreeReaderStream> makeTextIndexInputStream(
    DataPartStoragePtr data_part_storage,
    const String & stream_name,
    const String & extension,
    const MergeTreeReaderSettings & reader_settings)
{
    static constexpr size_t marks_count = 1;

    /// Check for both original and hashed filenames (hashed if the index name is too long)
    auto actual_stream_name = IMergeTreeDataPart::getStreamNameOrHash(stream_name, extension, *data_part_storage);
    if (!actual_stream_name)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "File for text index stream {} does not exist", stream_name + extension);

    /// Use reader stream that doesn't read marks,
    /// because text index always has one mark.
    return std::make_unique<MergeTreeReaderStreamSingleColumnWholePart>(
        data_part_storage,
        *actual_stream_name,
        extension,
        marks_count,
        MarkRanges{{0, marks_count}},
        reader_settings,
        /*uncompressed_cache=*/ nullptr,
        data_part_storage->getFileSize(*actual_stream_name + extension),
        /*marks_loader=*/ nullptr,
        ReadBufferFromFileBase::ProfileCallback{},
        CLOCK_MONOTONIC_COARSE);
}

}
