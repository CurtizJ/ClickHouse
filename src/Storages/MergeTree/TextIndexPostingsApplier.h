#pragma once
#include <Storages/MergeTree/MergeTreeIndexText.h>
#include <Storages/MergeTree/PostingListSegment.h>

#include <span>
#include <vector>

namespace DB
{

class IPostingListBlockCodec;

/// Targets that the posting list of one token is folded into while it is being read.
/// Every target belongs to a text search query that references the token, and the fold
/// keeps only the rows inside `readable_ranges`.
struct PostingsApplyTargets
{
    /// Row ids folded by intersection (`All` and `Phrase` queries): a sorted array of unique row ids
    /// inside the readable ranges. The token keeps the rows it contains and drops the others.
    struct Intersect
    {
        PaddedPODArray<UInt32> * rows = nullptr;
        /// False until the first token is folded: the readable postings of the token become the initial rows.
        bool initialized = false;
        /// Number of rows left after the token was applied.
        size_t num_applied = 0;
    };

    /// Row ids folded by union (`Any` queries) into a bitmap.
    struct Unite
    {
        PostingList * postings = nullptr;
        /// Number of readable row ids of the token merged into the bitmap.
        size_t num_applied = 0;
    };

    /// Row ranges still readable after the analysis of the primary key and prior skip indexes,
    /// sorted and disjoint. nullptr: every row is readable.
    const std::vector<RowsRange> * readable_ranges = nullptr;

    std::vector<Intersect> intersect;
    std::vector<Unite> unite;

    bool empty() const { return intersect.empty() && unite.empty(); }

    /// True if some target can use postings from the closed row range: the range overlaps the readable rows
    /// and a target takes every readable row, or the rows folded so far by an intersect target fall into it.
    /// Valid before the token is applied.
    bool needRange(size_t begin, size_t end) const;
};

/// Folds a posting list into `PostingsApplyTargets` block by block. The row ids are fed in ascending order,
/// either as the packed blocks of a compressed segment or as an already deserialized sorted array.
/// Only the blocks some target can use are decoded (see `PostingsApplyTargets::needRange`).
/// The intersect targets are compacted in place, so `finish` must be called after the last block.
class PostingsApplier
{
public:
    explicit PostingsApplier(PostingsApplyTargets & targets_);

    /// Folds one compressed segment of a posting list, decoding only the packed blocks that some target can use.
    void applySegment(const PostingListSegment & segment, IPostingListBlockCodec & block_codec);
    /// Folds a sorted array of unique row ids.
    void applyRows(std::span<const UInt32> sorted_rows);
    /// Folds a bitmap.
    void applyBitmap(const PostingList & postings);
    /// Drops the rows of the intersect targets that the token did not contain and fills `num_applied` of every target.
    void finish();

private:
    /// True if some target can use postings from the closed row range that follows the rows applied so far.
    bool needBlock(UInt32 begin, UInt32 end);
    /// True if every target has taken all rows it can: the remaining blocks need not be inspected.
    bool exhausted() const;
    /// Folds a sorted block of unique row ids that follows the rows applied so far.
    void applyBlock(const UInt32 * values, size_t count);
    /// Keeps the values inside the readable ranges. Returns the number of values written to `clip_buffer`.
    size_t clipToReadableRanges(const UInt32 * values, size_t count);

    PostingsApplyTargets & targets;

    /// True if a target takes every readable row of the token: a union target or a not yet initialized intersect target.
    bool has_readable_rows_targets = false;

    /// Read and write positions of the in-place compaction of every initialized intersect target.
    struct IntersectState
    {
        size_t read_pos = 0;
        size_t write_pos = 0;
    };
    std::vector<IntersectState> intersect_states;

    /// The first readable range that may contain the rows applied next.
    size_t readable_range_idx = 0;

    /// A decoded packed block and its rows inside the readable ranges.
    PaddedPODArray<UInt32> decode_buffer;
    PaddedPODArray<UInt32> clip_buffer;

    /// Readable rows of the token for the union targets. They are collected into a fresh bitmap and merged
    /// into the targets at `finish`: adding blocks of rows one by one into a bitmap that already holds
    /// the sparse array containers of an earlier token is much slower than a union of two bitmaps.
    PostingList union_postings;
    size_t num_union_applied = 0;

    size_t num_blocks_decoded = 0;
    size_t num_blocks_skipped = 0;
};

}
