#pragma once

#include <base/defines.h>

#include <fmt/format.h>

/// Multiple Boolean values. That is, two Boolean values: can it be true, can it be false.
///
/// The third component, `always_unknown`, marks a result that depends only on atoms the analysis cannot evaluate at all
/// (e.g. a condition on a column that is not part of the key, `KeyCondition::RPNElement::FUNCTION_UNKNOWN`). Such a
/// result can be both true and false, like an ordinary uncertain result, but unlike that one it cannot be resolved
/// by narrowing the analyzed range: it stays unknown on every subrange, so no subrange can be excluded. A consumer
/// that subdivides ranges until the result is certain (the generic exclusion search) may therefore stop at an
/// unknown result as if the condition were certainly true. `always_unknown` implies `can_be_true && can_be_false`,
/// so the consumers that only look at the two Boolean components are not affected by it.
struct BoolMask
{
    bool can_be_true = false;
    bool can_be_false = false;
    bool always_unknown = false;

    BoolMask() = default;
    BoolMask(bool can_be_true_, bool can_be_false_) : can_be_true(can_be_true_), can_be_false(can_be_false_) { }
    /// The result for an atom that the analysis cannot evaluate on any range.
    static BoolMask createAlwaysUnknown() { return {true, true, true}; }

    /// The condition is certainly true or unknown on the range: no subrange could be excluded, so a search that
    /// subdivides ranges until the result is certain may stop here. False for an impossible result and for an
    /// uncertain one that a subrange may still resolve.
    bool alwaysTrueOrUnknown() const { return !can_be_false || always_unknown; }
    bool canBeFalseOrAlwaysUnknown() const { return can_be_false || always_unknown; }

    /// The conjunction is unknown if one operand is unknown and the other one is certainly true or unknown as well.
    /// An impossible operand makes the conjunction impossible instead. An operand that a subrange may still resolve
    /// to false keeps the conjunction resolvable, because it becomes impossible on that subrange.
    BoolMask operator&(const BoolMask & m) const
    {
        bool result_can_be_true = can_be_true && m.can_be_true;
        bool result_can_be_false = can_be_false || m.can_be_false;

        bool result_is_unknown = result_can_be_true && result_can_be_false;
        bool result_always_unknown = result_is_unknown && ((always_unknown && m.alwaysTrueOrUnknown()) || (m.always_unknown && alwaysTrueOrUnknown()));

        return {result_can_be_true, result_can_be_false, result_always_unknown};
    }

    /// The disjunction is unknown if one operand is unknown and the other one is not certainly true. A certainly
    /// true operand makes the disjunction certainly true instead. No other operand matters: the disjunction can be
    /// true wherever the unknown operand can, so no subrange could be excluded.
    BoolMask operator|(const BoolMask & m) const
    {
        bool result_can_be_true = can_be_true || m.can_be_true;
        bool result_can_be_false = can_be_false && m.can_be_false;

        bool result_is_unknown = result_can_be_true && result_can_be_false;
        bool result_always_unknown = result_is_unknown && ((always_unknown && m.canBeFalseOrAlwaysUnknown()) || (m.always_unknown && canBeFalseOrAlwaysUnknown()));

        return {result_can_be_true, result_can_be_false, result_always_unknown};
    }

    /// The negation of an unknown result is unknown as well.
    BoolMask operator!() const { return {can_be_false, can_be_true, always_unknown}; }

    bool operator==(const BoolMask & other) const
    {
        return can_be_true == other.can_be_true && can_be_false == other.can_be_false && always_unknown == other.always_unknown;
    }

    /// Check if mask is no longer changeable under BoolMask::combine.
    /// We use this condition to early-exit KeyConditions::checkInRange methods.
    /// An unknown result is still changeable: a part where the condition is impossible would make it resolvable.
    bool isComplete() const
    {
        return can_be_true && can_be_false && !always_unknown;
    }

    /// Combine check result in different hyperrectangles.
    /// The union is unknown if one part is unknown and the other one is certainly true or unknown as well, the same
    /// rule as for a conjunction: a part where the condition is impossible or resolvable could be excluded or
    /// resolved by narrowing the range, so the search must go on.
    static BoolMask combine(const BoolMask & left, const BoolMask & right)
    {
        bool result_can_be_true = left.can_be_true || right.can_be_true;
        bool result_can_be_false = left.can_be_false || right.can_be_false;

        bool result_is_unknown = result_can_be_true && result_can_be_false;
        bool result_always_unknown = result_is_unknown && ((left.always_unknown && right.alwaysTrueOrUnknown()) || (right.always_unknown && left.alwaysTrueOrUnknown()));

        return {result_can_be_true, result_can_be_false, result_always_unknown};
    }

    /// The following two special constants are used to speed up
    /// KeyCondition::checkInRange. When used as an initial_mask argument, they
    /// effectively prevent calculation of discarded BoolMask component as it is
    /// no longer changeable under BoolMask::combine (isComplete).
    static const BoolMask consider_only_can_be_true;
    static const BoolMask consider_only_can_be_false;

private:
    /// Only the operators and `createAlwaysUnknown` produce masks with the third component, so the invariant
    /// `always_unknown` implies `can_be_true && can_be_false` cannot be broken from outside.
    BoolMask(bool can_be_true_, bool can_be_false_, bool always_unknown_)
        : can_be_true(can_be_true_), can_be_false(can_be_false_), always_unknown(always_unknown_)
    {
        chassert(!always_unknown || (can_be_true && can_be_false));
    }
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
        if (mask.always_unknown)
            return fmt::format_to(ctx.out(), "({}, {}, always unknown)", mask.can_be_true, mask.can_be_false);
        return fmt::format_to(ctx.out(), "({}, {})", mask.can_be_true, mask.can_be_false);
    }
};
}
