#include <DataTypes/DataTypesCache.h>
#include <DataTypes/DataTypeFactory.h>

namespace DB
{

SimpleDataTypeCache::SimpleDataTypeCache()
{
    addSimpleType(BinaryTypeIndex::Nothing, "Nothing");
    addSimpleType(BinaryTypeIndex::UInt8, "UInt8");
    addSimpleType(BinaryTypeIndex::UInt16, "UInt16");
    addSimpleType(BinaryTypeIndex::UInt32, "UInt32");
    addSimpleType(BinaryTypeIndex::UInt64, "UInt64");
    addSimpleType(BinaryTypeIndex::UInt128, "UInt128");
    addSimpleType(BinaryTypeIndex::UInt256, "UInt256");
    addSimpleType(BinaryTypeIndex::Int8, "Int8");
    addSimpleType(BinaryTypeIndex::Int16, "Int16");
    addSimpleType(BinaryTypeIndex::Int32, "Int32");
    addSimpleType(BinaryTypeIndex::Int64, "Int64");
    addSimpleType(BinaryTypeIndex::Int128, "Int128");
    addSimpleType(BinaryTypeIndex::Int256, "Int256");
    addSimpleType(BinaryTypeIndex::BFloat16, "BFloat16");
    addSimpleType(BinaryTypeIndex::Float32, "Float32");
    addSimpleType(BinaryTypeIndex::Float64, "Float64");
    addSimpleType(BinaryTypeIndex::Date, "Date");
    addSimpleType(BinaryTypeIndex::Date32, "Date32");
    addSimpleType(BinaryTypeIndex::String, "String");
    addSimpleType(BinaryTypeIndex::UUID, "UUID");
    addSimpleType(BinaryTypeIndex::IPv4, "IPv4");
    addSimpleType(BinaryTypeIndex::IPv6, "IPv6");
    addSimpleType(BinaryTypeIndex::Bool, "Bool");
}

void SimpleDataTypeCache::addSimpleType(BinaryTypeIndex index, const String & type_name)
{
    auto type = DataTypeFactory::instance().get(type_name);
    Element element{type_name, type, type->getDefaultSerialization()};
    by_index[static_cast<uint8_t>(index)] = element;
    by_name.emplace(type_name, std::move(element));
}

bool SimpleDataTypeCache::hasElement(BinaryTypeIndex index) const
{
    return by_index[static_cast<uint8_t>(index)].type != nullptr;
}

const SimpleDataTypeCache::Element & SimpleDataTypeCache::getElement(BinaryTypeIndex index) const
{
    return by_index[static_cast<uint8_t>(index)];
}

DataTypePtr SimpleDataTypeCache::getType(BinaryTypeIndex index) const
{
    return by_index[static_cast<uint8_t>(index)].type;
}

DataTypePtr SimpleDataTypeCache::getType(const String & type_name) const
{
    auto it = by_name.find(type_name);
    if (it != by_name.end())
        return it->second.type;

    return DataTypeFactory::instance().get(type_name);
}

SerializationPtr SimpleDataTypeCache::getSerialization(const String & type_name) const
{
    auto it = by_name.find(type_name);
    if (it != by_name.end())
        return it->second.serialization;

    auto type = DataTypeFactory::instance().get(type_name);
    return type->getDefaultSerialization();
}

const SimpleDataTypeCache & SimpleDataTypeCache::instance()
{
    static SimpleDataTypeCache cache;
    return cache;
}

const SimpleDataTypeCache & getSimpleDataTypeCache()
{
    return SimpleDataTypeCache::instance();
}

}
