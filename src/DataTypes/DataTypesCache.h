#pragma once

#include <DataTypes/IDataType.h>
#include <DataTypes/DataTypesBinaryEncoding.h>

#include <array>
#include <unordered_map>

namespace DB
{

class DataTypeFactory;

/// Cache of simple (parameterless) data types and their serializations,
/// pre-filled at construction time. Avoids repeated DataTypeFactory lookups
/// and shared_ptr allocations for commonly used types.
/// Thread-safe: immutable after construction.
class SimpleDataTypeCache
{
public:
    struct Element
    {
        String name;
        DataTypePtr type;
        SerializationPtr serialization;
    };

    static const SimpleDataTypeCache & instance();

    bool hasElement(BinaryTypeIndex index) const;

    /// O(1) lookup by BinaryTypeIndex. Returns the cached element.
    const Element & getElement(BinaryTypeIndex index) const;

    /// O(1) lookup by BinaryTypeIndex. Returns nullptr for non-simple types.
    DataTypePtr getType(BinaryTypeIndex index) const;

    /// Lookup by type name. Returns pre-cached type for simple types,
    /// falls back to DataTypeFactory for others.
    DataTypePtr getType(const String & type_name) const;

    /// Lookup serialization by type name. Returns pre-cached serialization
    /// for simple types, falls back to DataTypeFactory for others.
    SerializationPtr getSerialization(const String & type_name) const;

private:
    SimpleDataTypeCache();

    void addSimpleType(BinaryTypeIndex index, const String & type_name);

    std::array<Element, BINARY_TYPE_INDEX_SIZE> by_index{};
    std::unordered_map<String, Element> by_name;
};

/// Return the singleton instance of the simple data type cache.
const SimpleDataTypeCache & getSimpleDataTypeCache();

}
