#pragma once

#include <Common/Exception.h>
#include <Common/HashTable/Hash.h>
#include <Common/UTF8Helpers.h>
#include <Common/VectorWithMemoryTracking.h>
#include <Core/ColumnNumbers.h>
#include <Functions/FunctionHelpers.h>
#include <base/types.h>
#include <base/unaligned.h>
#include <bit>
#include <limits>
#include <optional>

namespace DB
{

namespace ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}

struct CRC32CHasher
{
    size_t operator()(const char* data, size_t length) const
    {
        return updateWeakHash32(reinterpret_cast<const UInt8*>(data), length, 0);
    }
};

using Pos = const char *;

template <bool is_utf8>
class SparseGramsImpl
{
private:
    /// Position of the next symbol: +1 byte, or the whole UTF-8 sequence in the UTF-8 flavor.
    static size_t nextPosition(Pos data, size_t length, size_t position)
    {
        if constexpr (is_utf8)
            return std::min(length, position + UTF8::seqLength(data[position]));
        else
            return position + 1;
    }

    struct SubString
    {
        size_t left_index;
        size_t right_index;
        size_t symbols_between;
    };

    CRC32CHasher hasher;

    Pos pos = nullptr;
    Pos end = nullptr;
    UInt64 min_ngram_length = 3;
    UInt64 max_ngram_length = 100;
    std::optional<UInt64> min_cutoff_length;

    /// Current batch of answers. The size of result can not be greater than `convex_hull`.
    /// The size of `convex_hull` should not be large, see comment to `convex_hull` for more details.
    VectorWithMemoryTracking<SubString> result;
    size_t iter_result = 0;

    struct PositionAndHash
    {
        size_t left_ngram_position;
        /// The last right symbol index for which this entry still produces a gram within
        /// max_ngram_length: symbol_index + (max_ngram_length - min_ngram_length + 1).
        /// Precomputed at push time, so the hot loops compare it directly with the current
        /// right symbol index; the gram length is derived back as
        /// right_symbol_index + max_ngram_length - expiry_symbol.
        size_t expiry_symbol;
        size_t hash;
    };

    class NGramSymbolIterator
    {
    public:
        NGramSymbolIterator() = default;

        NGramSymbolIterator(Pos data_, Pos end_, size_t n_)
            : data(data_), end(end_), n(n_)
        {
        }

        bool increment()
        {
            if (isEnd())
                return false;

            right_iterator = getNextPosition(right_iterator);

            if (++num_increments >= n)
                left_iterator = getNextPosition(left_iterator);

            return true;
        }

        bool isEnd() const
        {
            return data + right_iterator >= end;
        }

        std::pair<size_t, size_t> getNGramPositions() const
        {
            return {left_iterator, right_iterator};
        }

        size_t getRightSymbol() const
        {
            return num_increments;
        }

        size_t getNextPosition(size_t iterator) const
        {
            return nextPosition(data, end - data, iterator);
        }

    private:
        Pos data = nullptr;
        Pos end = nullptr;
        size_t n = 0;
        size_t right_iterator = 0;
        size_t left_iterator = 0;
        size_t num_increments = 0;
    };

    /// The convex hull contains the maximum values ​​of the suffixes that start from the current right iterator.
    /// For example, if we have n-gram hashes like [1,5,2,4,1,3] and current right position is 4 (the last one)
    /// than our convex hull will consists of elements:
    /// [{position:1, hash:5}, {position:3, hash:4}, {position:4,hash:1}]
    /// Assuming that hashes are uniformly distributed, the expected size of convex_hull is N^{1/3},
    /// where N is the length of the string.
    /// Proof: https://math.stackexchange.com/questions/3469295/expected-number-of-vertices-in-a-convex-hull
    VectorWithMemoryTracking<PositionAndHash> convex_hull;
    NGramSymbolIterator symbol_iterator;

    /// Get the next batch of answers: processes one right position of the gram window,
    /// pushes all grams anchored at it into `result` and advances the iterator.
    /// Returns false if there can be no more answers.
    /// This is the reference implementation of the traversal; the hot push path
    /// uses the equivalent fused loop in forEachGramImpl.
    bool consume()
    {
        if (symbol_iterator.isEnd())
            return false;

        auto [ngram_left_position, right_position] = symbol_iterator.getNGramPositions();
        size_t right_symbol_index = symbol_iterator.getRightSymbol();
        size_t next_right_position = symbol_iterator.getNextPosition(right_position);
        size_t right_border_ngram_hash = hasher(pos + ngram_left_position, next_right_position - ngram_left_position);

        while (!convex_hull.empty() && convex_hull.back().hash < right_border_ngram_hash)
        {
            size_t possible_left_position = convex_hull.back().left_ngram_position;
            size_t expiry_symbol = convex_hull.back().expiry_symbol;
            if (right_symbol_index > expiry_symbol)
            {
                /// The gram is longer than max_ngram_length and it will only become longer at future
                /// right positions; the entries below are older, so they are all expired too.
                convex_hull.clear();
                break;
            }
            result.push_back({
                .left_index = possible_left_position,
                .right_index = next_right_position,
                .symbols_between = right_symbol_index + max_ngram_length - expiry_symbol
            });
            convex_hull.pop_back();
        }

        if (!convex_hull.empty())
        {
            size_t possible_left_position = convex_hull.back().left_ngram_position;
            size_t expiry_symbol = convex_hull.back().expiry_symbol;
            if (right_symbol_index <= expiry_symbol)
                result.push_back({
                    .left_index = possible_left_position,
                    .right_index = next_right_position,
                    .symbols_between = right_symbol_index + max_ngram_length - expiry_symbol
                });
        }

        /// there should not be identical hashes in the convex hull. If there are, then we leave only the last one
        while (!convex_hull.empty() && convex_hull.back().hash == right_border_ngram_hash)
            convex_hull.pop_back();

        convex_hull.push_back(PositionAndHash{
            .left_ngram_position = ngram_left_position,
            .expiry_symbol = right_symbol_index + max_ngram_length - min_ngram_length + 1,
            .hash = right_border_ngram_hash
        });
        symbol_iterator.increment();
        return true;
    }

    /// Hash of the gram window. Produces exactly the same value as CRC32CHasher for any input;
    /// the dominant case (a 2-byte window: single-byte symbols with the default min_ngram_length = 3)
    /// is inlined to avoid the length dispatch inside updateWeakHash32.
    size_t windowHash(const char * data, size_t window_length) const
    {
        if constexpr (std::endian::native == std::endian::little)
        {
            if (window_length == 2)
            {
                /// Replicates the `size < 8` branch of updateWeakHash32:
                /// the low bytes of the word are the data, byte 7 is the length.
                UInt64 value = unalignedLoad<UInt16>(data);
                value |= UInt64(2) << 56;
                return static_cast<UInt32>(intHashCRC32(value, 0));
            }
        }
        return hasher(data, window_length);
    }

    /// The hot path of forEachGram: one flat loop with all state in registers.
    /// Produces exactly the same token stream as draining consume via set/get;
    /// the equivalence is checked by `tokenizers-benchmark --verify-mb`.
    ///
    /// The differences from consume are only mechanical:
    /// - the convex hull is accessed through local pointers with a sentinel at the bottom
    ///   (never popped: its hash exceeds any CRC32 value; never emitted: its expiry symbol is 0),
    ///   so the loops need no emptiness checks and never write the vector size back to memory;
    /// - the symbol iterator is inlined; in the main loop the left border advances on every step
    ///   (num_increments >= n always holds after the warmup), so the counter check disappears;
    /// - the next position of the right border is computed once per step and reused for the advance.
    template <bool has_cutoff, typename Callback>
    void forEachGramImpl(Callback && callback)
    {
        const char * data = pos;
        const size_t length = end - pos;

        /// Warmup, as in set: advance the right border to the (min - 1)-th symbol.
        size_t right_position = 0;
        for (size_t i = 0; i + 2 < min_ngram_length; ++i)
        {
            if (right_position >= length)
                return;
            right_position = nextPosition(data, length, right_position);
        }

        if (convex_hull.size() < 64)
            convex_hull.resize(64);

        PositionAndHash * hull_begin = convex_hull.data();
        PositionAndHash * hull_top = hull_begin;
        PositionAndHash * hull_storage_end = hull_begin + convex_hull.size();
        *hull_top = PositionAndHash{.left_ngram_position = 0, .expiry_symbol = 0, .hash = std::numeric_limits<size_t>::max()};

        const size_t expiry_delta = max_ngram_length - min_ngram_length + 1;
        /// length >= min_cutoff_length, rewritten via the expiry symbol:
        /// right_symbol + cutoff_slack >= expiry_symbol.
        const size_t cutoff_slack = has_cutoff ? max_ngram_length - *min_cutoff_length : 0;

        size_t left_position = 0;
        size_t right_symbol = min_ngram_length - 2;

        while (right_position < length)
        {
            const size_t next_right_position = nextPosition(data, length, right_position);
            const size_t hash = windowHash(data + left_position, next_right_position - left_position);

            while (hull_top->hash < hash)
            {
                if (right_symbol > hull_top->expiry_symbol)
                {
                    /// The gram is longer than max_ngram_length and it will only become longer at future
                    /// right positions; the entries below are older, so they are all expired too.
                    hull_top = hull_begin;
                    break;
                }
                if (!has_cutoff || right_symbol + cutoff_slack >= hull_top->expiry_symbol)
                {
                    if (callback(data + hull_top->left_ngram_position, data + next_right_position))
                        return;
                }
                --hull_top;
            }

            /// The top entry with hash >= ours also anchors a gram ending here (and stays in the hull).
            if (right_symbol <= hull_top->expiry_symbol)
            {
                if (!has_cutoff || right_symbol + cutoff_slack >= hull_top->expiry_symbol)
                {
                    if (callback(data + hull_top->left_ngram_position, data + next_right_position))
                        return;
                }
            }

            /// There should not be identical hashes in the hull; keep only the newest one.
            while (hull_top->hash == hash)
                --hull_top;

            if (hull_top + 1 == hull_storage_end)
            {
                /// Rare: the expected size of the hull is the cubic root of the string length.
                const size_t top_offset = hull_top - hull_begin;
                convex_hull.resize(convex_hull.size() * 2);
                hull_begin = convex_hull.data();
                hull_top = hull_begin + top_offset;
                hull_storage_end = hull_begin + convex_hull.size();
            }

            ++hull_top;
            hull_top->left_ngram_position = left_position;
            hull_top->expiry_symbol = right_symbol + expiry_delta;
            hull_top->hash = hash;

            right_position = next_right_position;
            left_position = nextPosition(data, length, left_position);
            ++right_symbol;
        }
    }

    std::optional<SubString> getNextIndices()
    {
        if (result.size() <= iter_result)
        {
            result.clear();
            iter_result = 0;

            if (!consume())
                return std::nullopt;

            return getNextIndices();
        }

        return result[iter_result++];
    }

public:
    static constexpr auto name = is_utf8 ? "sparseGramsUTF8" : "sparseGrams";
    static constexpr auto strings_argument_position = 0uz;
    static bool isVariadic() { return true; }
    static size_t getNumberOfArguments() { return 0; }
    static ColumnNumbers getArgumentsThatAreAlwaysConstant() { return {1}; }

    SparseGramsImpl() = default;
    explicit SparseGramsImpl(UInt64 min_ngram_length_, UInt64 max_ngram_length_, std::optional<UInt64> min_cutoff_length_)
        : min_ngram_length(min_ngram_length_)
        , max_ngram_length(max_ngram_length_)
        , min_cutoff_length(std::move(min_cutoff_length_))
    {
    }

    static void checkArguments(const IFunction & func, const ColumnsWithTypeAndName & arguments)
    {
        FunctionArgumentDescriptors mandatory_args{
            {"s", static_cast<FunctionArgumentDescriptor::TypeValidator>(&isString), nullptr, "String"},
        };

        FunctionArgumentDescriptors optional_args{
            {"min_ngram_length", static_cast<FunctionArgumentDescriptor::TypeValidator>(&isNativeInteger), isColumnConst, "const Number"},
            {"max_ngram_length", static_cast<FunctionArgumentDescriptor::TypeValidator>(&isNativeInteger), isColumnConst, "const Number"},
            {"min_cutoff_length", static_cast<FunctionArgumentDescriptor::TypeValidator>(&isNativeInteger), isColumnConst, "const Number"},
        };

        validateFunctionArguments(func, arguments, mandatory_args, optional_args);
    }

    void init(const ColumnsWithTypeAndName & arguments, bool /*max_substrings_includes_remaining_string*/)
    {
        if (arguments.size() > 4)
            throw Exception(
                ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                "Number of arguments for function {} doesn't match: passed {}, must be from 1 to 4",
                name,
                arguments.size());

        if (arguments.size() >= 2)
            min_ngram_length = arguments[1].column->getUInt(0);

        if (min_ngram_length < 3)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Argument 'min_ngram_length' must be greater or equal to 3");

        if (arguments.size() == 3)
            max_ngram_length = arguments[2].column->getUInt(0);

        if (max_ngram_length < min_ngram_length)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Argument 'max_ngram_length' must be greater or equal to 'min_ngram_length'");

        if (arguments.size() == 4)
            min_cutoff_length = arguments[3].column->getUInt(0);

        if (min_cutoff_length && *min_cutoff_length < min_ngram_length)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Argument 'min_cutoff_length' must be greater or equal to 'min_ngram_length'");

        if (min_cutoff_length && *min_cutoff_length > max_ngram_length)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Argument 'min_cutoff_length' must be less or equal to 'max_ngram_length'");
    }

    /// Called for each next string.
    void set(Pos pos_, Pos end_)
    {
        result.clear();
        convex_hull.clear();
        iter_result = 0;

        pos = pos_;
        end = end_;

        symbol_iterator = NGramSymbolIterator(pos, end, min_ngram_length - 1);
        for (size_t i = 0; i < min_ngram_length - 2; ++i)
            if (!symbol_iterator.increment())
                return;
    }

    /// Push-style counterpart of set/get: visits all sparse grams of [pos_, end_) in the same order
    /// as consecutive calls to get would return them and calls `callback(token_begin, token_end)` for each one.
    /// Stops early if the callback returns true.
    /// Does not materialize the `result` batch, so it is preferred for hot paths (see forEachToken in ITokenizer.h).
    template <typename Callback>
    void forEachGram(Pos pos_, Pos end_, Callback && callback)
    {
        /// Reset the pull-side state for consistency (the batched result is not used by the push path).
        result.clear();
        iter_result = 0;

        pos = pos_;
        end = end_;

        if (min_cutoff_length)
            forEachGramImpl</*has_cutoff=*/ true>(callback);
        else
            forEachGramImpl</*has_cutoff=*/ false>(callback);
    }

    /// Get the next token, if any, or return false.
    bool get(Pos & token_begin, Pos & token_end)
    {
        while (true)
        {
            auto cur_result = getNextIndices();
            if (!cur_result)
                return false;

            auto iter_left = cur_result->left_index;
            auto iter_right = cur_result->right_index;
            auto length = cur_result->symbols_between;

            if (min_cutoff_length && *min_cutoff_length > length)
                continue;

            token_begin = pos + iter_left;
            token_end = pos + iter_right;
            return true;
        }
    }
};

}
