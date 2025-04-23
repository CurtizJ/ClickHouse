#include <Functions/FunctionFactory.h>
#include <Functions/geometryConverters.h>

#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>

#include <Columns/ColumnTuple.h>
#include <Columns/ColumnsNumber.h>
#include <DataTypes/DataTypesNumber.h>
#include <boost/geometry/strategy/geographic/area.hpp>
#include "Common/PODArray_fwd.h"
#include "Functions/FunctionHelpers.h"
#include "base/types.h"

#include <memory>
#include <string>

namespace DB
{

namespace ErrorCodes
{
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
}

namespace
{

struct CartesianAreaImpl
{
    using Point = CartesianPoint;
    static constexpr size_t num_arguments = 1;
    static constexpr auto name = "polygonAreaCartesian";
};

struct SphericalAreaImpl
{
    using Point = SphericalPoint;
    static constexpr size_t num_arguments = 0;
    static constexpr auto name = "polygonAreaSpherical";
};

struct GeographicAreaImpl
{
    using Point = SphericalPoint;
    static constexpr size_t num_arguments = 1;
    static constexpr auto name = "polygonAreaGeographic";
};

template <typename Impl>
class FunctionPolygonArea : public IFunction
{
public:
    using Point = typename Impl::Point;
    static inline const char * name = Impl::name;

    explicit FunctionPolygonArea() = default;

    static FunctionPtr create(ContextPtr)
    {
        return std::make_shared<FunctionPolygonArea>();
    }

    String getName() const override
    {
        return name;
    }

    bool useDefaultImplementationForConstants() const override
    {
        return true;
    }

    bool isVariadic() const override
    {
        return Impl::num_arguments == 0;
    }

    size_t getNumberOfArguments() const override
    {
        return Impl::num_arguments;
    }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        if constexpr (std::is_same_v<Impl, SphericalAreaImpl>)
        {
            if (arguments.empty() || arguments.size() > 2)
                throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                    "Function {} requires at most two arguments: polygon and optional radius of the sphere", getName());

            if (arguments.size() == 2 && !isNumber(arguments[1].type) && !isColumnConst(*arguments[1].column))
                throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                    "The second argument of function {} must be a number, got: {}", getName(), arguments[1].type->getName());
        }

        return std::make_shared<DataTypeFloat64>();
    }

    DataTypePtr getReturnTypeForDefaultImplementationForDynamic() const override
    {
        return std::make_shared<DataTypeFloat64>();
    }

    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return true; }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & /*result_type*/, size_t input_rows_count) const override
    {
        auto res_column = ColumnFloat64::create();
        auto & res_data = res_column->getData();
        res_data.reserve(input_rows_count);

        callOnGeometryDataType<Point>(arguments[0].type, [&] (const auto & type)
        {
            using TypeConverter = std::decay_t<decltype(type)>;
            using Converter = typename TypeConverter::Type;

            if constexpr (std::is_same_v<ColumnToPointsConverter<Point>, Converter>)
            {
                throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT, "The argument of function {} must not be Point", getName());
            }
            else
            {
                auto geometries = Converter::convert(arguments[0].column->convertToFullColumnIfConst());

                if constexpr (std::is_same_v<Impl, GeographicAreaImpl>)
                {
                    boost::geometry::strategy::area::geographic<> strategy_geographic;
                    executeWithStrategy(res_data, geometries, strategy_geographic);
                    return;
                }

                if constexpr (std::is_same_v<Impl, SphericalAreaImpl>)
                {
                    if (arguments.size() == 2)
                    {
                        if (!arguments[1].column->isNumeric())
                            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                                "The second argument of function {} must be a constant number, got: {}", getName(), arguments[1].column->getName());

                        Float64 radius = arguments[1].column->getFloat64(0);
                        boost::geometry::strategy::area::spherical<> strategy_spherical(radius);
                        executeWithStrategy(res_data, geometries, strategy_spherical);
                        return;
                    }
                }

                executeWithoutStrategy(res_data, geometries);
            }
        });

        return res_column;
    }

    template <typename Geometries>
    void executeWithoutStrategy(PaddedPODArray<Float64> & res_data, const Geometries & geometries) const
    {
        for (const auto & geometry : geometries)
            res_data.emplace_back(boost::geometry::area(geometry));
    }

    template <typename Geometries, typename Strategy>
    void executeWithStrategy(PaddedPODArray<Float64> & res_data, const Geometries & geometries, const Strategy & strategy) const
    {
        for (const auto & geometry : geometries)
            res_data.emplace_back(boost::geometry::area(geometry, strategy));
    }
};

}

REGISTER_FUNCTION(PolygonArea)
{
    factory.registerFunction<FunctionPolygonArea<CartesianAreaImpl>>();
    factory.registerFunction<FunctionPolygonArea<SphericalAreaImpl>>();
    factory.registerFunction<FunctionPolygonArea<GeographicAreaImpl>>();
}

}
