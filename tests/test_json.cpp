#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include <lob/json.hpp>

namespace lob::json {
namespace {

TEST(JsonReader, WalksAFlatObject) {
  Reader r(R"({"a":1,"b":"two","c":true,"d":null,"e":3.5})");
  ASSERT_TRUE(r.EnterObject());

  std::string_view key;
  std::int64_t a = 0;
  std::string_view b;
  bool c = false;
  double e = 0.0;

  ASSERT_TRUE(r.NextMember(key));
  EXPECT_EQ(key, "a");
  ASSERT_TRUE(r.ReadInt(a));
  ASSERT_TRUE(r.NextMember(key));
  EXPECT_EQ(key, "b");
  ASSERT_TRUE(r.ReadStringRaw(b));
  ASSERT_TRUE(r.NextMember(key));
  EXPECT_EQ(key, "c");
  ASSERT_TRUE(r.ReadBool(c));
  ASSERT_TRUE(r.NextMember(key));
  EXPECT_EQ(key, "d");
  ASSERT_TRUE(r.ReadNull());
  ASSERT_TRUE(r.NextMember(key));
  EXPECT_EQ(key, "e");
  ASSERT_TRUE(r.ReadDouble(e));
  EXPECT_FALSE(r.NextMember(key));
  EXPECT_TRUE(r.ok());

  EXPECT_EQ(a, 1);
  EXPECT_EQ(b, "two");
  EXPECT_TRUE(c);
  EXPECT_DOUBLE_EQ(e, 3.5);
}

TEST(JsonReader, ReadsBinanceStyleQuotedNumbers) {
  // Every numeric market-data field Binance sends is a JSON *string*.
  Reader r(R"({"p":"99999.99","q":"0.00120000","u":"12345"})");
  ASSERT_TRUE(r.EnterObject());
  std::string_view key;
  double p = 0.0;
  double q = 0.0;
  std::int64_t u = 0;
  ASSERT_TRUE(r.NextMember(key));
  ASSERT_TRUE(r.ReadNumberLoose(p));
  ASSERT_TRUE(r.NextMember(key));
  ASSERT_TRUE(r.ReadNumberLoose(q));
  ASSERT_TRUE(r.NextMember(key));
  ASSERT_TRUE(r.ReadIntLoose(u));
  EXPECT_DOUBLE_EQ(p, 99999.99);
  EXPECT_DOUBLE_EQ(q, 0.0012);
  EXPECT_EQ(u, 12345);
}

TEST(JsonReader, SkipsNestedValuesOfEveryShape) {
  Reader r(R"({"keep":7,"drop":{"a":[1,2,{"b":null}],"c":"x"},"also":[[1],[2,3]],"tail":9})");
  ASSERT_TRUE(r.EnterObject());
  std::string_view key;
  std::int64_t keep = 0;
  std::int64_t tail = 0;
  while (r.NextMember(key)) {
    if (key == "keep") {
      ASSERT_TRUE(r.ReadInt(keep));
    } else if (key == "tail") {
      ASSERT_TRUE(r.ReadInt(tail));
    } else {
      ASSERT_TRUE(r.SkipValue()) << "failed to skip key " << key;
    }
  }
  EXPECT_TRUE(r.ok()) << r.error();
  EXPECT_EQ(keep, 7);
  EXPECT_EQ(tail, 9);
}

TEST(JsonReader, ReadsNestedArraysOfLevels) {
  // The exact shape of the diff-depth "b" / "a" arrays.
  Reader r(R"([["100.00","5.0"],["99.99","0"]])");
  ASSERT_TRUE(r.EnterArray());

  std::vector<std::pair<double, double>> levels;
  while (r.NextElement()) {
    ASSERT_TRUE(r.EnterArray());
    double px = 0.0;
    double qty = 0.0;
    ASSERT_TRUE(r.NextElement());
    ASSERT_TRUE(r.ReadNumberLoose(px));
    ASSERT_TRUE(r.NextElement());
    ASSERT_TRUE(r.ReadNumberLoose(qty));
    EXPECT_FALSE(r.NextElement());
    levels.emplace_back(px, qty);
  }
  ASSERT_TRUE(r.ok()) << r.error();
  ASSERT_EQ(levels.size(), 2u);
  EXPECT_DOUBLE_EQ(levels[0].first, 100.00);
  EXPECT_DOUBLE_EQ(levels[0].second, 5.0);
  EXPECT_DOUBLE_EQ(levels[1].second, 0.0);
}

TEST(JsonReader, HandlesEmptyContainers) {
  Reader r(R"({"b":[],"a":{}})");
  ASSERT_TRUE(r.EnterObject());
  std::string_view key;
  ASSERT_TRUE(r.NextMember(key));
  ASSERT_TRUE(r.EnterArray());
  EXPECT_FALSE(r.NextElement());
  ASSERT_TRUE(r.NextMember(key));
  ASSERT_TRUE(r.EnterObject());
  std::string_view inner;
  EXPECT_FALSE(r.NextMember(inner));
  EXPECT_FALSE(r.NextMember(key));
  EXPECT_TRUE(r.ok());
}

TEST(JsonReader, UnescapesStringsOnDemand) {
  Reader r(R"({"s":"a\"b\\c\ndéA"})");
  ASSERT_TRUE(r.EnterObject());
  std::string_view key;
  ASSERT_TRUE(r.NextMember(key));
  std::string out;
  ASSERT_TRUE(r.ReadStringCopy(out));
  EXPECT_EQ(out, std::string("a\"b\\c\nd\xc3\xa9""A"));
}

TEST(JsonReader, ErrorsAreStickyAndReported) {
  Reader r(R"({"a":})");
  ASSERT_TRUE(r.EnterObject());
  std::string_view key;
  ASSERT_TRUE(r.NextMember(key));
  EXPECT_FALSE(r.SkipValue());
  EXPECT_FALSE(r.ok());
  EXPECT_FALSE(r.error().empty());
  // Every subsequent call is a no-op that keeps returning false.
  std::int64_t v = 0;
  EXPECT_FALSE(r.ReadInt(v));
  EXPECT_FALSE(r.NextMember(key));
}

TEST(JsonReader, RejectsTrailingCommas) {
  Reader r(R"({"a":1,})");
  ASSERT_TRUE(r.EnterObject());
  std::string_view key;
  std::int64_t v = 0;
  ASSERT_TRUE(r.NextMember(key));
  ASSERT_TRUE(r.ReadInt(v));
  EXPECT_FALSE(r.NextMember(key));
  EXPECT_FALSE(r.ok());
}

TEST(ParseHelpers, ParseWholeStringOrFail) {
  double d = 0.0;
  EXPECT_TRUE(ParseDouble("-12.5e2", d));
  EXPECT_DOUBLE_EQ(d, -1250.0);
  EXPECT_FALSE(ParseDouble("12abc", d));
  EXPECT_FALSE(ParseDouble("", d));

  std::int64_t i = 0;
  EXPECT_TRUE(ParseInt("-42", i));
  EXPECT_EQ(i, -42);
  EXPECT_FALSE(ParseInt("4.2", i));
  EXPECT_FALSE(ParseInt("", i));
}

}  // namespace
}  // namespace lob::json
