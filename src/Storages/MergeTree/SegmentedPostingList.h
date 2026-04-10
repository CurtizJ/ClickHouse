#pragma once

#include <Common/Exception.h>
#include <base/defines.h>
#include <base/types.h>
#include <roaring/roaring.hh>

#include <algorithm>
#include <vector>


namespace DB
{

/// A posting list that transparently handles >4B row IDs by splitting
/// into fixed-size segments, each backed by a roaring::Roaring bitmap.
/// Segment size is constant per index. All values are absolute (UInt64);
/// internally stored as segment-local UInt32 in the appropriate segment.
///
/// For single-segment parts (<=4B rows), the wrapper degenerates to one
/// Roaring bitmap with near-zero overhead.
class SegmentedPostingList
{
public:
    static constexpr size_t DEFAULT_SEGMENT_SIZE = static_cast<size_t>(1) << 32;

    SegmentedPostingList()
        : segment_size_(DEFAULT_SEGMENT_SIZE)
    {
    }

    explicit SegmentedPostingList(size_t segment_size)
        : segment_size_(segment_size)
    {
    }

    /// Construct from raw UInt32 values (all placed in segment 0).
    SegmentedPostingList(size_t n, const UInt32 * vals)
        : segment_size_(DEFAULT_SEGMENT_SIZE)
    {
        if (n > 0)
        {
            ensureSegment(0);
            segments_[0].postings.addMany(n, vals);
        }
    }

    /// Insert absolute value.
    void add(UInt64 value)
    {
        size_t idx = segmentIndex(value);
        ensureSegment(idx);
        segments_[idx].postings.add(segmentLocal(value));
    }

    /// Insert with per-segment BulkContext optimization for sequential inserts.
    void addBulk(UInt64 value)
    {
        size_t idx = segmentIndex(value);
        ensureSegment(idx);
        segments_[idx].postings.addBulk(segments_[idx].bulk_context, segmentLocal(value));
    }

    /// Add half-open range [min, max).
    void addRange(UInt64 min, UInt64 max)
    {
        if (min >= max)
            return;

        size_t first_seg = segmentIndex(min);
        size_t last_seg = segmentIndex(max - 1);
        ensureSegment(last_seg);

        for (size_t seg = first_seg; seg <= last_seg; ++seg)
        {
            UInt64 seg_start = static_cast<UInt64>(seg) * segment_size_;
            UInt64 seg_end = seg_start + segment_size_;
            UInt64 range_begin = std::max(min, seg_start);
            UInt64 range_end = std::min(max, seg_end);
            segments_[seg].postings.addRange(
                static_cast<UInt32>(range_begin - seg_start),
                static_cast<UInt32>(range_end - seg_start));
        }
    }

    /// Add closed range [min, max].
    void addRangeClosed(UInt64 min, UInt64 max)
    {
        addRange(min, max + 1);
    }

    UInt64 cardinality() const
    {
        UInt64 total = 0;
        for (const auto & seg : segments_)
            total += seg.postings.cardinality();
        return total;
    }

    bool isEmpty() const
    {
        for (const auto & seg : segments_)
            if (!seg.postings.isEmpty())
                return false;
        return true;
    }

    UInt64 minimum() const
    {
        for (size_t i = 0; i < segments_.size(); ++i)
            if (!segments_[i].postings.isEmpty())
                return toAbsolute(i, segments_[i].postings.minimum());
        return 0;
    }

    UInt64 maximum() const
    {
        for (size_t i = segments_.size(); i > 0; --i)
            if (!segments_[i - 1].postings.isEmpty())
                return toAbsolute(i - 1, segments_[i - 1].postings.maximum());
        return 0;
    }

    /// AND — applied per-segment independently.
    SegmentedPostingList operator&(const SegmentedPostingList & other) const
    {
        SegmentedPostingList result(segment_size_);
        size_t common = std::min(segments_.size(), other.segments_.size());
        result.segments_.resize(common);
        for (size_t i = 0; i < common; ++i)
            result.segments_[i].postings = segments_[i].postings & other.segments_[i].postings;
        return result;
    }

    /// OR — applied per-segment independently.
    SegmentedPostingList operator|(const SegmentedPostingList & other) const
    {
        SegmentedPostingList result(segment_size_);
        size_t max_size = std::max(segments_.size(), other.segments_.size());
        result.segments_.resize(max_size);
        for (size_t i = 0; i < max_size; ++i)
        {
            if (i < segments_.size() && i < other.segments_.size())
                result.segments_[i].postings = segments_[i].postings | other.segments_[i].postings;
            else if (i < segments_.size())
                result.segments_[i].postings = segments_[i].postings;
            else
                result.segments_[i].postings = other.segments_[i].postings;
        }
        return result;
    }

    SegmentedPostingList & operator|=(const SegmentedPostingList & other)
    {
        if (segments_.size() < other.segments_.size())
            segments_.resize(other.segments_.size());
        for (size_t i = 0; i < other.segments_.size(); ++i)
            segments_[i].postings |= other.segments_[i].postings;
        return *this;
    }

    SegmentedPostingList & operator&=(const SegmentedPostingList & other)
    {
        size_t common = std::min(segments_.size(), other.segments_.size());
        for (size_t i = 0; i < common; ++i)
            segments_[i].postings &= other.segments_[i].postings;
        /// Segments beyond other's range become empty.
        for (size_t i = common; i < segments_.size(); ++i)
            segments_[i].postings = roaring::Roaring();
        return *this;
    }

    /// Extract all values as absolute UInt64.
    void toUint64Array(UInt64 * out) const
    {
        size_t offset = 0;
        for (size_t seg = 0; seg < segments_.size(); ++seg)
        {
            const auto & postings = segments_[seg].postings;
            UInt64 card = postings.cardinality();
            if (card == 0)
                continue;

            std::vector<UInt32> local(card);
            postings.toUint32Array(local.data());
            for (size_t i = 0; i < card; ++i)
                out[offset + i] = toAbsolute(seg, local[i]);
            offset += card;
        }
    }

    /// Extract as UInt32. Only valid for single-segment posting lists.
    void toUint32Array(UInt32 * out) const
    {
        chassert(isSingleSegment());
        if (!segments_.empty())
            segments_[0].postings.toUint32Array(out);
    }

    void runOptimize()
    {
        for (auto & seg : segments_)
            seg.postings.runOptimize();
    }

    void clear()
    {
        segments_.clear();
    }

    size_t getSizeInBytes() const
    {
        size_t total = sizeof(*this);
        for (const auto & seg : segments_)
            total += seg.postings.getSizeInBytes();
        return total;
    }

    size_t numSegments() const { return segments_.size(); }
    size_t segmentSize() const { return segment_size_; }

    roaring::Roaring & segment(size_t idx)
    {
        ensureSegment(idx);
        return segments_[idx].postings;
    }

    const roaring::Roaring & segment(size_t idx) const
    {
        static const roaring::Roaring empty;
        if (idx < segments_.size())
            return segments_[idx].postings;
        return empty;
    }

    bool isSingleSegment() const
    {
        return segments_.size() <= 1;
    }

    UInt64 toAbsolute(size_t seg_idx, UInt32 local) const
    {
        return static_cast<UInt64>(seg_idx) * segment_size_ + local;
    }

    /// Read from Roaring portable format (single-segment compat).
    static SegmentedPostingList read(const char * buf)
    {
        SegmentedPostingList result;
        result.ensureSegment(0);
        result.segments_[0].postings = roaring::Roaring::read(buf);
        return result;
    }

private:
    size_t segment_size_;

    struct Segment
    {
        roaring::Roaring postings;
        roaring::BulkContext bulk_context;

        Segment() = default;
        Segment(Segment && other) noexcept = default;
        Segment & operator=(Segment && other) noexcept = default;

        /// Copy constructor: copy the Roaring, reset BulkContext.
        Segment(const Segment & other) : postings(other.postings) {}
        Segment & operator=(const Segment & other)
        {
            if (this != &other)
            {
                postings = other.postings;
                bulk_context = roaring::BulkContext();
            }
            return *this;
        }
    };

    std::vector<Segment> segments_;

    size_t segmentIndex(UInt64 v) const { return static_cast<size_t>(v / segment_size_); }
    UInt32 segmentLocal(UInt64 v) const { return static_cast<UInt32>(v % segment_size_); }

    void ensureSegment(size_t idx)
    {
        if (idx >= segments_.size())
            segments_.resize(idx + 1);
    }
};

}
