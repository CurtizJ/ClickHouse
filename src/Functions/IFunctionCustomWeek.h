#pragma once
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDate32.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <Functions/CustomWeekTransforms.h>
#include <Functions/IFunction.h>
#include <Functions/TransformDateTime64.h>
#include <IO/WriteHelpers.h>
#include <Interpreters/Context.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
}

template <typename Transform>
class IFunctionCustomWeek : public IFunction
{
public:
    static constexpr auto name = Transform::name;
    String getName() const override { return name; }
    bool isVariadic() const override { return true; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return true; }
    size_t getNumberOfArguments() const override { return 0; }
    bool useDefaultImplementationForConstants() const override { return true; }
    ColumnNumbers getArgumentsThatAreAlwaysConstant() const override { return {1, 2}; }

    bool hasInformationAboutMonotonicity() const override { return true; }

    Monotonicity getMonotonicityForRange(const IDataType & type, const Field & left, const Field & right) const override
    {
        if constexpr (std::is_same_v<typename Transform::FactorTransform, ZeroTransform>)
            return {.is_monotonic = true, .is_always_monotonic = true};

        const IFunction::Monotonicity is_monotonic = {.is_monotonic = true};
        const IFunction::Monotonicity is_not_monotonic;

        /// This method is called only if the function has one argument. Therefore, we do not care about the non-local time zone.
        const DateLUTImpl & date_lut = DateLUT::instance();

        if (left.isNull() || right.isNull())
            return {};

        /// The function is monotonous on the [left, right] segment, if the factor transformation returns the same values for them.

        if (checkAndGetDataType<DataTypeDate>(&type))
        {
            return Transform::FactorTransform::execute(UInt16(left.get<UInt64>()), date_lut)
                    == Transform::FactorTransform::execute(UInt16(right.get<UInt64>()), date_lut)
                ? is_monotonic
                : is_not_monotonic;
        }
        else
        {
            return Transform::FactorTransform::execute(UInt32(left.get<UInt64>()), date_lut)
                    == Transform::FactorTransform::execute(UInt32(right.get<UInt64>()), date_lut)
                ? is_monotonic
                : is_not_monotonic;
        }
    }

protected:
    void checkArguments(const ColumnsWithTypeAndName & arguments, bool is_result_type_date_or_date32 = false) const
    {
        if (arguments.size() == 1)
        {
            if (!isDate(arguments[0].type) && !isDate32(arguments[0].type) && !isDateTime(arguments[0].type) && !isDateTime64(arguments[0].type))
                throw Exception(
                    "Illegal type " + arguments[0].type->getName() + " of argument of function " + getName()
                        + ". Must be Date, Date32, DateTime or DateTime64.",
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
        }
        else if (arguments.size() == 2)
        {
            if (!isDate(arguments[0].type) && !isDate32(arguments[0].type) && !isDateTime(arguments[0].type) && !isDateTime64(arguments[0].type))
                throw Exception(
                    "Illegal type " + arguments[0].type->getName() + " of 1st argument of function " + getName()
                        + ". Must be Date, Date32, DateTime or DateTime64.",
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
            if (!isUInt8(arguments[1].type))
                throw Exception(
                    "Illegal type of 2nd (optional) argument of function " + getName()
                        + ". Must be constant UInt8 (week mode).",
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
        }
        else if (arguments.size() == 3)
        {
            if (!isDate(arguments[0].type) && !isDate32(arguments[0].type) && !isDateTime(arguments[0].type) && !isDateTime64(arguments[0].type))
                throw Exception(
                    "Illegal type " + arguments[0].type->getName() + " of argument of function " + getName()
                        + ". Must be Date, Date32, DateTime or DateTime64",
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
            if (!isUInt8(arguments[1].type))
                throw Exception(
                    "Illegal type of 2nd (optional) argument of function " + getName()
                        + ". Must be constant UInt8 (week mode).",
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
            if (!isString(arguments[2].type))
                throw Exception(
                    "Illegal type of 3rd (optional) argument of function " + getName()
                        + ". Must be constant string (timezone name).",
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
            if ((isDate(arguments[0].type) || isDate32(arguments[0].type)) && is_result_type_date_or_date32)
                throw Exception(
                    "The timezone argument of function " + getName() + " is allowed only when the 1st argument is DateTime or DateTime64.",
                    ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
        }
        else
            throw Exception(
                "Number of arguments for function " + getName() + " doesn't match: passed " + toString(arguments.size())
                    + ", expected 1, 2 or 3.",
                ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);
    }

};

}
