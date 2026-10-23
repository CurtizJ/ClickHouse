#include <Storages/MergeTree/IPostingListCodec.h>
#include <Storages/MergeTree/MergeTreeIndexTextPostingListCodec.h>
#include <IO/Operators.h>

#include <roaring/containers/containers.h>
#include <roaring/roaring_array.h>

#include <algorithm>

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

std::unique_ptr<IPostingListCodec> PostingListCodecFactory::createPostingListCodec(IPostingListCodec::Type type)
{
    switch (type)
    {
        case IPostingListCodec::Type::None:
            return std::make_unique<PostingListCodecNone>();
        case IPostingListCodec::Type::Bitpacking:
            return std::make_unique<PostingListCodecBitpacking>();
        case IPostingListCodec::Type::PFor:
            return std::make_unique<PostingListCodecPFor>();
    }

    throw Exception(ErrorCodes::BAD_ARGUMENTS, "Unknown posting list codec type: {}", static_cast<int>(type));
}

std::unique_ptr<IPostingListCodec> PostingListCodecFactory::createPostingListCodec(std::string_view codec_name, const String & caller_name)
{
    if (codec_name == "none")
        return createPostingListCodec(IPostingListCodec::Type::None);

    if (codec_name == PostingListCodecBitpacking::getName())
        return createPostingListCodec(IPostingListCodec::Type::Bitpacking);

    if (codec_name == PostingListCodecPFor::getName())
        return createPostingListCodec(IPostingListCodec::Type::PFor);

    throw Exception(ErrorCodes::BAD_ARGUMENTS, "Unknown posting list codec: '{}' for index '{}'", codec_name, caller_name);
}

PostingList buildPostingListFromSortedRowIds(std::span<const UInt32> row_ids)
{
    using namespace roaring::internal;

    PostingList postings;
    roaring::roaring_array_t * containers = &postings.roaring.high_low_container;

    const UInt32 * begin = row_ids.data();
    const UInt32 * const end = begin + row_ids.size();

    while (begin != end)
    {
        /// The row ids of a container share the high 16 bits.
        const UInt16 key = static_cast<UInt16>(*begin >> 16);
        const UInt32 * container_end = key == std::numeric_limits<UInt16>::max()
            ? end
            : std::lower_bound(begin, end, (static_cast<UInt32>(key) + 1) << 16);
        const auto count = static_cast<size_t>(container_end - begin);

        /// The containers must be appended with growing keys. Unsorted row ids of corrupted data
        /// may break that, so such a container is inserted the slow way.
        if (ra_get_size(containers) > 0 && containers->keys[ra_get_size(containers) - 1] >= key)
        {
            postings.addMany(count, begin);
        }
        else if (count > DEFAULT_MAX_SIZE)
        {
            bitset_container_t * container = bitset_container_create();
            UInt64 * words = container->words;
            UInt32 word_idx = 0;
            UInt64 word = 0;

            /// Sorted row ids fill every word in one run, so the bits of the run are accumulated in a register and
            /// stored without reading the word back. Unsorted row ids of corrupted data may lose bits, but stay in bounds.
            for (const UInt32 * it = begin; it != container_end; ++it)
            {
                const auto low = static_cast<UInt16>(*it);
                const UInt32 current_word_idx = low >> 6;
                word = (current_word_idx == word_idx ? word : 0) | (UInt64(1) << (low & 63));
                word_idx = current_word_idx;
                words[word_idx] = word;
            }

            /// Counted rather than set to `count`, which includes the duplicate row ids of corrupted data.
            container->cardinality = bitset_container_compute_cardinality(container);
            ra_append(containers, key, container, BITSET_CONTAINER_TYPE);
        }
        else
        {
            array_container_t * container = array_container_create_given_capacity(static_cast<int32_t>(count));
            for (size_t i = 0; i < count; ++i)
                container->array[i] = static_cast<UInt16>(begin[i]);

            container->cardinality = static_cast<int32_t>(count);
            ra_append(containers, key, container, ARRAY_CONTAINER_TYPE);
        }

        begin = container_end;
    }

    return postings;
}

}
