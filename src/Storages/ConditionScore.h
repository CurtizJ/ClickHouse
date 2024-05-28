#pragma once
#include <Core/Types.h>

namespace DB
{

struct ConditionScore
{
    UInt64 columns_size = 0;
    UInt64 num_table_columns = 0;

    /// Can condition be moved to prewhere?
    bool viable = false;

    /// Does the condition presumably have good selectivity?
    bool good = false;

    /// the lower the better
    Float64 selectivity = 1.0;

    /// Does the condition contain primary key column?
    /// If so, it is better to move it further to the end of PREWHERE chain depending on minimal position in PK of any
    /// column in this condition because this condition have bigger chances to be already satisfied by PK analysis.
    static constexpr Int64 INF_POSITION = std::numeric_limits<Int64>::max() - 1;
    Int64 min_position_in_primary_key = INF_POSITION;

    auto tuple() const
    {
        return std::make_tuple(!viable, -min_position_in_primary_key, selectivity, columns_size, num_table_columns);
    }

    /// Is condition a better candidate for moving to PREWHERE?
    bool operator< (const ConditionScore & rhs) const
    {
        return tuple() < rhs.tuple();
    }

    bool hasColumnInPrimaryKey() const
    {
        return min_position_in_primary_key != INF_POSITION;
    }
};

}
