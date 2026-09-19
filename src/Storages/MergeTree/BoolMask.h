#pragma once

#include <base/defines.h>

#include <fmt/format.h>

/// Multiple Boolean values. That is, two Boolean values: can it be true, can it be false.
///
/// The third component, `unknown`, marks a result that depends only on atoms the analysis cannot evaluate at all
/// (e.g. a condition on a column that is not part of the key, `KeyCondition::RPNElement::FUNCTION_UNKNOWN`). Such a
/// result can be both true and false, like an ordinary uncertain result, but unlike that one it cannot be resolved
/// by narrowing the analyzed range: it stays unknown on every subrange, so no subrange can be excluded. A consumer
/// that subdivides ranges until the result is certain (the generic exclusion search) may therefore stop at an
/// unknown result as if the condition were certainly true. `unknown` implies `can_be_true && can_be_false`, so the
/// consumers that only look at the two Boolean components are not affected by it.
struct BoolMask
{
    bool can_be_true = false;
    bool can_be_false = false;
    bool unknown = false;

    BoolMask() = default;
    BoolMask(bool can_be_true_, bool can_be_false_) : can_be_true(can_be_true_), can_be_false(can_be_false_) { }
    BoolMask(bool can_be_true_, bool can_be_false_, bool unknown_)
        : can_be_true(can_be_true_), can_be_false(can_be_false_), unknown(unknown_)
    {
        chassert(!unknown || (can_be_true && can_be_false));
    }

    /// Whether a search that subdivides ranges until the result is certain may stop at this result: the condition
    /// is either certainly true or unknown on the range, so no subrange could be excluded.
    bool isCertainOrUnknown() const { return !can_be_false || unknown; }

    /// An impossible operand makes the conjunction impossible; an uncertain operand that a subrange may still
    /// resolve keeps it resolvable; otherwise an unknown operand makes it unknown.
    BoolMask operator&(const BoolMask & m) const
    {
        bool result_can_be_true = can_be_true && m.can_be_true;
        bool result_can_be_false = can_be_false || m.can_be_false;
        bool result_unknown = result_can_be_true && (unknown || m.unknown) && isCertainOrUnknown() && m.isCertainOrUnknown();
        return {result_can_be_true, result_can_be_false, result_unknown};
    }

    /// A certainly true operand makes the disjunction certainly true; otherwise an unknown operand makes it unknown,
    /// because the disjunction can be true wherever the unknown operand is.
    BoolMask operator|(const BoolMask & m) const
    {
        bool result_can_be_true = can_be_true || m.can_be_true;
        bool result_can_be_false = can_be_false && m.can_be_false;
        bool result_unknown = result_can_be_false && (unknown || m.unknown);
        return {result_can_be_true, result_can_be_false, result_unknown};
    }

    /// The negation of an unknown result is unknown as well.
    BoolMask operator!() const { return {can_be_false, can_be_true, unknown}; }

    bool operator==(const BoolMask & other) const
    {
        return can_be_true == other.can_be_true && can_be_false == other.can_be_false && unknown == other.unknown;
    }

    /// Check if mask is no longer changeable under BoolMask::combine.
    /// We use this condition to early-exit KeyConditions::checkInRange methods.
    /// An unknown result is still changeable: a part where the condition is impossible would make it resolvable.
    bool isComplete() const
    {
        return can_be_true && can_be_false && !unknown;
    }

    /// Combine check result in different hyperrectangles.
    /// The union is unknown only if the search may stop at every part: a part where the condition is impossible or
    /// resolvable could be excluded or resolved by narrowing the range.
    static BoolMask combine(const BoolMask & left, const BoolMask & right)
    {
        bool result_can_be_true = left.can_be_true || right.can_be_true;
        bool result_can_be_false = left.can_be_false || right.can_be_false;
        bool result_unknown = result_can_be_false && left.isCertainOrUnknown() && right.isCertainOrUnknown();
        return {result_can_be_true, result_can_be_false, result_unknown};
    }

    /// The following two special constants are used to speed up
    /// KeyCondition::checkInRange. When used as an initial_mask argument, they
    /// effectively prevent calculation of discarded BoolMask component as it is
    /// no longer changeable under BoolMask::combine (isComplete).
    static const BoolMask consider_only_can_be_true;
    static const BoolMask consider_only_can_be_false;
};

namespace fmt
{
template <>
struct formatter<BoolMask>
{
    static constexpr auto parse(format_parse_context & ctx) { return ctx.begin(); }

    template <typename FormatContext>
    auto format(const BoolMask & mask, FormatContext & ctx)
    {
        if (mask.unknown)
            return fmt::format_to(ctx.out(), "({}, {}, unknown)", mask.can_be_true, mask.can_be_false);
        return fmt::format_to(ctx.out(), "({}, {})", mask.can_be_true, mask.can_be_false);
    }
};
}
