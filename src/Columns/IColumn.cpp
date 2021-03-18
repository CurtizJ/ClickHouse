#include <IO/WriteBufferFromString.h>
#include <IO/Operators.h>
#include <Columns/IColumn.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnConst.h>
#include <Core/Field.h>


namespace DB
{

String IColumn::dumpStructure() const
{
    WriteBufferFromOwnString res;
    res << getFamilyName() << "(size = " << size();

    ColumnCallback callback = [&](ColumnPtr & subcolumn)
    {
        res << ", " << subcolumn->dumpStructure();
    };

    const_cast<IColumn*>(this)->forEachSubcolumn(callback);

    res << ")";
    return res.str();
}

void IColumn::getEqualRanges(std::vector<size_t> & equal_ranges, size_t limit) const
{
    UNUSED(limit);
    // EqualRanges new_ranges;
    // new_ranges.reserve(equal_ranges.size());
    // for (const auto & range : equal_ranges)
    // {
    //     const auto & [first, last] = range;
    //     size_t begin = first;
    //     for (size_t i = first + 1; i < last; ++i)
    //     {
    //         if (compareAt(i - 1, i, *this, 1))
    //         {
    //             new_ranges.emplace_back(begin, i);
    //             begin = i;
    //         }
    //     }

    //     new_ranges.emplace_back(begin, last);
    // }

    // equal_ranges = std::move(new_ranges);
    UNUSED(equal_ranges);
}

void IColumn::insertFrom(const IColumn & src, size_t n)
{
    insert(src[n]);
}

bool isColumnNullable(const IColumn & column)
{
    return checkColumn<ColumnNullable>(column);
}

bool isColumnConst(const IColumn & column)
{
    return checkColumn<ColumnConst>(column);
}

}
