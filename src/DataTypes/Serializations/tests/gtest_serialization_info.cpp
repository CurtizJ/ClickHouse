#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <Columns/IColumn.h>
#include <Core/Block.h>
#include <Core/Field.h>
#include <Core/NamesAndTypes.h>
#include <DataTypes/Serializations/ISerialization.h>
#include <DataTypes/Serializations/SerializationInfo.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypeTuple.h>
#include <IO/WriteBufferFromString.h>
#include <Poco/JSON/Object.h>
#include <Common/Exception.h>

namespace DB
{

namespace ErrorCodes
{
extern const int LOGICAL_ERROR;
}

namespace
{

SerializationInfoSettings defaultSettings()
{
    SerializationInfoSettings s;
    s.ratio_of_defaults_for_sparse = 0.9375;
    s.choose_kind = true;
    return s;
}

Poco::JSON::Object makeKindObj(const std::string & kind, size_t num_rows, size_t num_defaults)
{
    Poco::JSON::Object obj;
    obj.set("kind", kind);
    obj.set("num_rows", static_cast<Poco::UInt64>(num_rows));
    obj.set("num_defaults", static_cast<Poco::UInt64>(num_defaults));
    return obj;
}

void expectMalformedKindFails([[maybe_unused]] const std::string & kind)
{
#ifdef DEBUG_OR_SANITIZER_BUILD
    GTEST_SKIP() << "this test trigger LOGICAL_ERROR, runs only if DEBUG_OR_SANITIZER_BUILD is not defined";
#else
    EXPECT_THROW(
        {
            try
            {
                SerializationInfo info({ISerialization::Kind::DEFAULT}, defaultSettings());
                auto obj = makeKindObj(kind, 100, 10);
                info.fromJSON(obj);
            }
            catch (const DB::Exception & e)
            {
                ASSERT_EQ(DB::ErrorCodes::LOGICAL_ERROR, e.code());
                EXPECT_THAT(e.what(), testing::HasSubstr("Unknown serialization kind"));
                throw;
            }
        },
        DB::Exception);
#endif
}
}

/// `SerializationInfo` is now a pure kind descriptor: `toJSON`/`fromJSON` round-trip only the
/// serialization kind. The per-column row/default counts (used to choose sparse serialization) live
/// in statistics and are read into an `Estimate` via `fromJSONWithStats` for legacy on-disk formats.

TEST(SerializationInfoJSON, ToFromJSONRoundTripsKindDefault)
{
    ISerialization::KindStack kind_stack{ISerialization::Kind::DEFAULT};
    SerializationInfo info(kind_stack, defaultSettings());

    Poco::JSON::Object obj;
    info.toJSON(obj);
    EXPECT_EQ(obj.getValue<std::string>("kind"), "Default");

    SerializationInfo restored({ISerialization::Kind::DEFAULT}, defaultSettings());
    restored.fromJSON(obj);
    EXPECT_EQ(restored.getKindStack(), kind_stack);
}

TEST(SerializationInfoJSON, ToFromJSONRoundTripsKindSparse)
{
    ISerialization::KindStack kind_stack{ISerialization::Kind::DEFAULT, ISerialization::Kind::SPARSE};
    SerializationInfo info(kind_stack, defaultSettings());

    Poco::JSON::Object obj;
    info.toJSON(obj);
    EXPECT_EQ(obj.getValue<std::string>("kind"), "Sparse");

    SerializationInfo restored({ISerialization::Kind::DEFAULT}, defaultSettings());
    restored.fromJSON(obj);
    EXPECT_EQ(restored.getKindStack(), kind_stack);
}

TEST(SerializationInfoJSON, ToFromJSONRoundTripsKindDetachedOverSparse)
{
    /// kindStackToString reverses the non-Default elements:
    ///   [Default, Sparse, Detached] -> "DetachedOverSparse"
    /// stringToKindStack reads left-to-right:
    ///   "DetachedOverSparse" -> [Default, Detached, Sparse]
    /// So the round-trip reverses the inner order.
    ISerialization::KindStack kind_stack{ISerialization::Kind::DEFAULT, ISerialization::Kind::SPARSE, ISerialization::Kind::DETACHED};
    SerializationInfo info(kind_stack, defaultSettings());

    Poco::JSON::Object obj;
    info.toJSON(obj);
    EXPECT_EQ(obj.getValue<std::string>("kind"), "DetachedOverSparse");

    SerializationInfo restored({ISerialization::Kind::DEFAULT}, defaultSettings());
    restored.fromJSON(obj);

    ISerialization::KindStack expected_after_roundtrip{
        ISerialization::Kind::DEFAULT, ISerialization::Kind::DETACHED, ISerialization::Kind::SPARSE};
    EXPECT_EQ(restored.getKindStack(), expected_after_roundtrip);
}

TEST(SerializationInfoJSON, FromJSONRequiresOnlyKind)
{
    SerializationInfo info({ISerialization::Kind::DEFAULT}, defaultSettings());

    /// `num_rows`/`num_defaults` are no longer required by `fromJSON` (they moved to statistics).
    Poco::JSON::Object only_kind;
    only_kind.set("kind", "Default");
    EXPECT_NO_THROW(info.fromJSON(only_kind));

    /// `kind` is still mandatory.
    Poco::JSON::Object no_kind;
    no_kind.set("num_rows", 100);
    no_kind.set("num_defaults", 10);
    EXPECT_THROW(info.fromJSON(no_kind), DB::Exception);
}

TEST(SerializationInfoJSON, FromJSONWithStatsReadsRowsAndDefaults)
{
    SerializationInfo info({ISerialization::Kind::DEFAULT}, defaultSettings());
    auto obj = makeKindObj("Sparse", 2000, 1999);

    Estimate stats;
    info.fromJSONWithStats(obj, stats);

    ISerialization::KindStack expected{ISerialization::Kind::DEFAULT, ISerialization::Kind::SPARSE};
    EXPECT_EQ(info.getKindStack(), expected);
    EXPECT_EQ(stats.rows_count, 2000u);
    ASSERT_TRUE(stats.estimated_defaults.has_value());
    EXPECT_EQ(*stats.estimated_defaults, 1999u);
    EXPECT_TRUE(stats.types.contains(StatisticsType::Basic));
}

TEST(SerializationInfoJSON, FromJSONWithStatsMissingFieldThrows)
{
    SerializationInfo info({ISerialization::Kind::DEFAULT}, defaultSettings());

    {
        Poco::JSON::Object obj;
        obj.set("kind", "Default");
        obj.set("num_rows", 100);
        /// missing num_defaults
        Estimate stats;
        EXPECT_THROW(info.fromJSONWithStats(obj, stats), DB::Exception);
    }

    {
        Poco::JSON::Object obj;
        obj.set("kind", "Default");
        obj.set("num_defaults", 10);
        /// missing num_rows
        Estimate stats;
        EXPECT_THROW(info.fromJSONWithStats(obj, stats), DB::Exception);
    }

    {
        Poco::JSON::Object obj;
        obj.set("num_rows", 100);
        obj.set("num_defaults", 10);
        /// missing kind
        Estimate stats;
        EXPECT_THROW(info.fromJSONWithStats(obj, stats), DB::Exception);
    }
}

TEST(SerializationInfoJSON, WriteJSONWithStatsEmitsRowsAndDefaults)
{
    SerializationInfo info({ISerialization::Kind::DEFAULT, ISerialization::Kind::SPARSE}, defaultSettings());

    Estimate stats;
    stats.rows_count = 1000;
    stats.estimated_defaults = 950;

    WriteBufferFromOwnString out;
    String name = "col";
    info.writeJSONWithStats(out, &name, stats);
    auto json = out.str();

    EXPECT_THAT(json, testing::HasSubstr(R"("kind":"Sparse")"));
    EXPECT_THAT(json, testing::HasSubstr(R"("name":"col")"));
    EXPECT_THAT(json, testing::HasSubstr(R"("num_defaults":950)"));
    EXPECT_THAT(json, testing::HasSubstr(R"("num_rows":1000)"));
}

TEST(SerializationInfoJSON, WriteJSONOmitsRowsAndDefaults)
{
    SerializationInfo info({ISerialization::Kind::DEFAULT}, defaultSettings());

    WriteBufferFromOwnString out;
    String name = "col";
    info.writeJSON(out, &name);
    auto json = out.str();

    EXPECT_THAT(json, testing::HasSubstr(R"("kind":"Default")"));
    EXPECT_THAT(json, testing::HasSubstr(R"("name":"col")"));
    EXPECT_THAT(json, testing::Not(testing::HasSubstr("num_defaults")));
    EXPECT_THAT(json, testing::Not(testing::HasSubstr("num_rows")));
}

/// Malformed kind tests.
/// stringToKind throws LOGICAL_ERROR which aborts in debug builds
/// but throws a catchable exception in release builds.

TEST(SerializationInfoJSON, FromJSONEmptyKind)
{
    expectMalformedKindFails("");
}
TEST(SerializationInfoJSON, FromJSONUnknownKind)
{
    expectMalformedKindFails("FooBar");
}
TEST(SerializationInfoJSON, FromJSONKindWrongCase)
{
    expectMalformedKindFails("sparse");
}
TEST(SerializationInfoJSON, FromJSONKindAllCaps)
{
    expectMalformedKindFails("SPARSE");
}
TEST(SerializationInfoJSON, FromJSONKindWithTrailingOver)
{
    expectMalformedKindFails("SparseOver");
}
TEST(SerializationInfoJSON, FromJSONKindWithLeadingOver)
{
    expectMalformedKindFails("OverSparse");
}
TEST(SerializationInfoJSON, FromJSONKindDoubleOver)
{
    expectMalformedKindFails("DetachedOverOverSparse");
}
TEST(SerializationInfoJSON, FromJSONKindWithWhitespace)
{
    expectMalformedKindFails(" Sparse");
}
TEST(SerializationInfoJSON, FromJSONKindJustOver)
{
    expectMalformedKindFails("Over");
}

TEST(SerializationInfoByNameJSON, WriteJSONCanBeReadBack)
{
    SerializationInfoSettings settings;
    settings.ratio_of_defaults_for_sparse = 0.5;
    settings.choose_kind = true;
    settings.version = MergeTreeSerializationInfoVersion::WITHOUT_DATA;
    settings.string_serialization_version = MergeTreeStringSerializationVersion::WITH_SIZE_STREAM;
    settings.propagate_types_serialization_versions_to_nested_types = true;

    auto string_type = std::make_shared<DataTypeString>();
    NamesAndTypesList columns
    {
        {"string\"with\\escapes", string_type},
        {"tuple", std::make_shared<DataTypeTuple>(DataTypes{string_type, string_type}, Strings{"a", "b"})},
    };

    SerializationInfoByName infos(columns, settings);

    WriteBufferFromOwnString out;
    infos.writeJSON(out);
    auto json = out.str();

    EXPECT_THAT(json, testing::HasSubstr(R"("name":"string\"with\\escapes")"));
    EXPECT_THAT(json, testing::HasSubstr(R"("subcolumns")"));
    EXPECT_THAT(json, testing::HasSubstr(R"("types_serialization_versions")"));
    /// In the WITHOUT_DATA version, per-column row/default counts are not stored.
    EXPECT_THAT(json, testing::Not(testing::HasSubstr("num_defaults")));

    auto result = loadSerializationInfosFromString(json);
    EXPECT_EQ(result.infos.getVersion(), MergeTreeSerializationInfoVersion::WITHOUT_DATA);
    EXPECT_EQ(result.infos.getSettings().string_serialization_version, MergeTreeStringSerializationVersion::WITH_SIZE_STREAM);
    EXPECT_TRUE(result.infos.getSettings().propagate_types_serialization_versions_to_nested_types);
    EXPECT_NE(result.infos.tryGet("string\"with\\escapes"), nullptr);
    EXPECT_NE(result.infos.tryGet("tuple"), nullptr);
}

/// For versions before WITHOUT_DATA, `writeSerializationInfosJSON` must emit per-column row/default
/// counts so the part stays readable (`fromJSONWithStats` requires them). This round-trip would throw
/// "Missed field 'num_defaults'" if only the kind were written.
TEST(SerializationInfoByNameJSON, WriteWithStatsForLegacyVersionCanBeReadBack)
{
    SerializationInfoSettings settings;
    settings.ratio_of_defaults_for_sparse = 0.5;
    settings.choose_kind = true;
    settings.version = MergeTreeSerializationInfoVersion::WITH_TYPES;
    settings.string_serialization_version = MergeTreeStringSerializationVersion::WITH_SIZE_STREAM;

    auto string_type = std::make_shared<DataTypeString>();
    auto column = string_type->createColumn();
    for (size_t i = 0; i < 10; ++i)
        column->insert(i < 6 ? Field("") : Field("value"));
    Block block{ColumnWithTypeAndName{std::move(column), string_type, "s"}};

    auto statistics_for_serializations = getImplicitStatisticsForSparseSerialization(block, settings);
    statistics_for_serializations.build(block);
    auto infos = loadSerializationInfosFromStatistics(statistics_for_serializations, settings);

    WriteBufferFromOwnString out;
    writeSerializationInfosJSON(out, infos, statistics_for_serializations.getEstimates());
    auto json = out.str();

    EXPECT_THAT(json, testing::HasSubstr("num_rows"));
    EXPECT_THAT(json, testing::HasSubstr("num_defaults"));

    auto result = loadSerializationInfosFromString(json);
    EXPECT_EQ(result.infos.getVersion(), MergeTreeSerializationInfoVersion::WITH_TYPES);
    EXPECT_NE(result.infos.tryGet("s"), nullptr);
    ASSERT_TRUE(result.stats.has_value());
}

/// For versions before WITHOUT_DATA the persisted `num_defaults` must be the real count computed from
/// the written data (the implicit statistics), not a value derived from the chosen serialization kind.
/// A dense column has fewer defaults than the sparse threshold, so a kind-based encoding would write 0;
TEST(SerializationInfoByNameJSON, WriteWithStatsUsesRealDefaultCount)
{
    SerializationInfoSettings settings;
    settings.ratio_of_defaults_for_sparse = 0.9375;
    settings.choose_kind = true;
    settings.version = MergeTreeSerializationInfoVersion::WITH_TYPES;

    auto string_type = std::make_shared<DataTypeString>();
    auto column = string_type->createColumn();
    constexpr size_t num_rows = 100;
    constexpr size_t num_defaults = 30; /// 30% defaults is below the sparse threshold => dense kind
    for (size_t i = 0; i < num_rows; ++i)
        column->insert(i < num_defaults ? Field("") : Field("value"));

    Block block{ColumnWithTypeAndName{std::move(column), string_type, "s"}};

    auto statistics_for_serializations = getImplicitStatisticsForSparseSerialization(block, settings);
    statistics_for_serializations.build(block);
    auto infos = loadSerializationInfosFromStatistics(statistics_for_serializations, settings);

    /// The column is dense: a kind-based synthesis would write num_defaults = 0 for it.
    ASSERT_NE(infos.tryGet("s"), nullptr);
    EXPECT_FALSE(ISerialization::hasKind(infos.getKindStack("s"), ISerialization::Kind::SPARSE));

    WriteBufferFromOwnString out;
    writeSerializationInfosJSON(out, infos, statistics_for_serializations.getEstimates());

    auto result = loadSerializationInfosFromString(out.str());
    ASSERT_TRUE(result.stats.has_value());
    auto it = result.stats->find("s");
    ASSERT_NE(it, result.stats->end());
    EXPECT_EQ(it->second.rows_count, num_rows);
    /// The real default count is persisted, not the kind-synthesized 0.
    EXPECT_GT(it->second.estimated_defaults.value_or(0), 0u);
}

}
