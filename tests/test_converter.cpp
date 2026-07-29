// Binance decoding, the book-sync protocol and binary event I/O
// (master plan §2.4, §4.3 -- CLAUDE.md Phase 1 gate).
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <lob/converter/binance.hpp>
#include <lob/converter/converter.hpp>
#include <lob/converter/event_io.hpp>

#include "test_support.hpp"

namespace lob {
namespace {

Instrument Inst() { return Instrument("BTCUSDT", 0, 0.01, 0.001); }

std::string DepthLine(Ts recv_us, long long event_ms, long long first_id, long long final_id,
                      const std::string& extra = {}) {
  return R"({"t":)" + std::to_string(recv_us) + R"(,"c":"depth","s":"BTCUSDT","d":{)" +
         R"("e":"depthUpdate","E":)" + std::to_string(event_ms) + R"(,"s":"BTCUSDT","U":)" +
         std::to_string(first_id) + R"(,"u":)" + std::to_string(final_id) + extra +
         R"(,"b":[["100.00","5.0"]],"a":[["100.01","4.0"]]}})";
}

// ---------------------------------------------------------------------------
// Envelope and payloads
// ---------------------------------------------------------------------------
TEST(BinanceEnvelope, ParsesTheRecorderLineFormat) {
  const std::string line =
      R"({"t":1722240000123456,"c":"aggTrade","s":"BTCUSDT","d":{"e":"aggTrade","a":7}})";
  binance::RecorderLine env;
  std::string error;
  ASSERT_TRUE(binance::ParseRecorderLine(line, env, &error)) << error;
  EXPECT_EQ(env.recv_ts_us, 1722240000123456LL);
  EXPECT_EQ(env.channel, binance::Channel::kAggTrade);
  EXPECT_EQ(env.symbol, "BTCUSDT");
  EXPECT_EQ(env.payload, R"({"e":"aggTrade","a":7})");
}

TEST(BinanceEnvelope, UnwrapsCombinedStreamPayloads) {
  const std::string line =
      R"({"t":1,"c":"depth","s":"BTCUSDT","d":{"stream":"btcusdt@depth","data":{"e":"x","u":9}}})";
  binance::RecorderLine env;
  ASSERT_TRUE(binance::ParseRecorderLine(line, env, nullptr));
  EXPECT_EQ(env.payload, R"({"e":"x","u":9})");
}

TEST(BinanceDepth, ProducesOneAbsoluteQuantityEventPerLevel) {
  const std::string payload =
      R"({"e":"depthUpdate","E":1568014460893,"s":"BTCUSDT","U":1,"u":9,)"
      R"("b":[["7403.89","0.002"],["7403.90","0.000"]],"a":[["7405.96","3.340"]]})";
  binance::DepthUpdate update;
  std::vector<Event> events;
  std::string error;
  ASSERT_TRUE(binance::ParseDepthUpdate(payload, Inst(), update, events, &error)) << error;

  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(update.exch_ts_us, 1568014460893LL * 1000);
  EXPECT_EQ(update.first_id, 1);
  EXPECT_EQ(update.final_id, 9);
  EXPECT_FALSE(update.has_prev_id);

  EXPECT_EQ(events[0].SideOf(), Side::kBid);
  EXPECT_EQ(events[0].price_ticks, 740389);
  EXPECT_EQ(events[0].qty_lots, 2);
  // A quantity of zero is a level DELETE, not an empty update.
  EXPECT_EQ(events[1].qty_lots, 0);
  EXPECT_EQ(events[2].SideOf(), Side::kAsk);
  EXPECT_EQ(events[2].qty_lots, 3340);
  // Every level in one message carries that message's final update id.
  for (const Event& e : events) {
    EXPECT_EQ(e.seq, 9);
    EXPECT_EQ(e.exch_ts_us, update.exch_ts_us);
  }
}

TEST(BinanceDepth, PrefersTransactionTimeOverPushTime) {
  // On USD-M futures, T is the match-engine time and E is when the event was
  // pushed.  Using E would attribute market moves later than they happened.
  const std::string payload =
      R"({"e":"depthUpdate","E":2000,"T":1500,"s":"BTCUSDT","U":1,"u":2,"pu":0,)"
      R"("b":[],"a":[]})";
  binance::DepthUpdate update;
  std::vector<Event> events;
  ASSERT_TRUE(binance::ParseDepthUpdate(payload, Inst(), update, events, nullptr));
  EXPECT_EQ(update.exch_ts_us, 1500 * 1000);
  EXPECT_TRUE(update.has_prev_id);
  EXPECT_EQ(update.prev_final_id, 0);
}

TEST(BinanceAggTrade, InvertsTheMakerFlagToGetTheAggressor) {
  // THE sign trap of the whole project.  m = "is the buyer the market maker".
  //   m = true  -> the resting order was a BID -> the AGGRESSOR was SELLING.
  const std::string maker_buyer =
      R"({"e":"aggTrade","E":1,"T":1000,"s":"BTCUSDT","a":42,"p":"100.00","q":"0.5","m":true})";
  binance::AggTrade t;
  ASSERT_TRUE(binance::ParseAggTrade(maker_buyer, Inst(), t, nullptr));
  EXPECT_EQ(t.aggressor, Side::kAsk);
  EXPECT_EQ(t.price_ticks, 10000);
  EXPECT_EQ(t.qty_lots, 500);
  EXPECT_EQ(t.agg_id, 42);
  EXPECT_EQ(t.exch_ts_us, 1'000'000);

  const std::string maker_seller =
      R"({"e":"aggTrade","E":1,"T":1000,"s":"BTCUSDT","a":43,"p":"100.00","q":"0.5","m":false})";
  ASSERT_TRUE(binance::ParseAggTrade(maker_seller, Inst(), t, nullptr));
  EXPECT_EQ(t.aggressor, Side::kBid);
}

TEST(BinanceAggTrade, RefusesATradeWithNoAggressorSide) {
  // A trade whose aggressor we cannot identify cannot fill a queue-tracked
  // order.  Dropping it quietly would bias fills, so it is a hard error.
  const std::string no_flag =
      R"({"e":"aggTrade","E":1,"T":1000,"s":"BTCUSDT","a":1,"p":"100.00","q":"0.5"})";
  binance::AggTrade t;
  std::string error;
  EXPECT_FALSE(binance::ParseAggTrade(no_flag, Inst(), t, &error));
  EXPECT_NE(error.find("'m'"), std::string::npos);
}

TEST(BinanceSnapshot, BracketsLevelsWithBeginAndEndMarkers) {
  const std::string payload =
      R"({"lastUpdateId":1027024,"bids":[["100.00","5.0"],["99.99","8.0"]],)"
      R"("asks":[["100.01","4.0"]]})";
  binance::DepthSnapshot snap;
  std::vector<Event> events;
  std::string error;
  ASSERT_TRUE(binance::ParseDepthSnapshot(payload, Inst(), 555, snap, events, &error)) << error;

  ASSERT_EQ(events.size(), 5u);
  EXPECT_EQ(events.front().Type(), EventType::kSnapshotBegin);
  EXPECT_EQ(events.back().Type(), EventType::kSnapshotEnd);
  EXPECT_EQ(snap.last_update_id, 1027024);
  EXPECT_EQ(snap.bid_levels, 2u);
  EXPECT_EQ(snap.ask_levels, 1u);
  // Spot REST responses carry no timestamp, so the recorder's receive time is
  // used and every record in the snapshot shares it.
  for (const Event& e : events) {
    EXPECT_EQ(e.exch_ts_us, 555);
    EXPECT_EQ(e.seq, 1027024);
  }
}

TEST(BinanceBookTicker, ParsesTheBbo) {
  const std::string payload =
      R"({"u":400900217,"s":"BTCUSDT","b":"100.00","B":"31.21","a":"100.02","A":"40.66"})";
  binance::BookTicker bt;
  ASSERT_TRUE(binance::ParseBookTicker(payload, Inst(), bt, nullptr));
  EXPECT_EQ(bt.bid_ticks, 10000);
  EXPECT_EQ(bt.ask_ticks, 10002);
  EXPECT_EQ(bt.update_id, 400900217);
}

// ---------------------------------------------------------------------------
// The sync protocol
// ---------------------------------------------------------------------------
ConverterOptions Options() {
  ConverterOptions o;
  o.instrument = Inst();
  return o;
}

std::string SnapshotLine(Ts recv_us, long long last_update_id) {
  return R"({"t":)" + std::to_string(recv_us) +
         R"(,"c":"snapshot","s":"BTCUSDT","d":{"lastUpdateId":)" +
         std::to_string(last_update_id) +
         R"(,"bids":[["100.00","5.0"]],"asks":[["100.01","4.0"]]}})";
}

TEST(Converter, DropsDiffsThatArriveBeforeAnySnapshot) {
  // Diffs describe changes to a state we do not have; applying them would
  // silently corrupt the book (Part 11 pitfall #4).
  Converter c(Options());
  c.ProcessLine(DepthLine(1000, 1, 10, 12), 1);
  EXPECT_EQ(c.stats().depth_dropped_presync, 1u);
  EXPECT_FALSE(c.synced());
  EXPECT_EQ(c.stats().events_emitted, 0u);
}

TEST(Converter, DiscardsDiffsEntirelyBelowTheSnapshot) {
  Converter c(Options());
  c.ProcessLine(SnapshotLine(1000, 100), 1);
  ASSERT_TRUE(c.synced());
  // §2.4: "discard buffered diffs entirely below that id".
  c.ProcessLine(DepthLine(1100, 2, 90, 95), 2);
  EXPECT_EQ(c.stats().depth_dropped_stale, 1u);
  EXPECT_EQ(c.gaps().size(), 0u);
}

TEST(Converter, AcceptsAFirstDiffThatBracketsLastUpdateIdPlusOne) {
  Converter c(Options());
  c.ProcessLine(SnapshotLine(1000, 100), 1);
  // U <= 101 <= u
  c.ProcessLine(DepthLine(1100, 2, 99, 105), 2);
  EXPECT_TRUE(c.gaps().empty());
  EXPECT_FALSE(c.dirty());
}

TEST(Converter, FlagsAFirstDiffThatDoesNotBracketLastUpdateIdPlusOne) {
  Converter c(Options());
  c.ProcessLine(SnapshotLine(1000, 100), 1);
  c.ProcessLine(DepthLine(1100, 2, 150, 160), 2);  // a hole between 101 and 150
  ASSERT_EQ(c.gaps().size(), 1u);
  EXPECT_EQ(c.gaps().front().expected, 101);
  EXPECT_TRUE(c.dirty());
}

TEST(Converter, DetectsASpotSequenceGapAndMarksTheIntervalDirty) {
  Converter c(Options());
  c.ProcessLine(SnapshotLine(1000, 100), 1);
  c.ProcessLine(DepthLine(1100, 2, 101, 105), 2);
  c.ProcessLine(DepthLine(1200, 3, 106, 110), 3);  // contiguous
  EXPECT_TRUE(c.gaps().empty());

  c.ProcessLine(DepthLine(1300, 4, 200, 205), 4);  // U != previous u + 1
  ASSERT_EQ(c.gaps().size(), 1u);
  EXPECT_EQ(c.gaps().front().expected, 111);
  EXPECT_EQ(c.gaps().front().got, 200);
  EXPECT_TRUE(c.dirty());

  // Dirty events are still emitted -- the book must stay live -- but flagged.
  const std::vector<Event> events = c.Finish();
  bool saw_dirty = false;
  for (const Event& e : events) {
    saw_dirty |= e.IsDirty();
  }
  EXPECT_TRUE(saw_dirty);
  EXPECT_GT(c.stats().events_dirty, 0u);
}

TEST(Converter, UsesThePreviousUpdateIdRuleOnFutures) {
  Converter c(Options());
  c.ProcessLine(SnapshotLine(1000, 100), 1);
  c.ProcessLine(DepthLine(1100, 2, 101, 105, R"(,"pu":100)"), 2);
  // pu must equal the previous message's u.
  c.ProcessLine(DepthLine(1200, 3, 106, 110, R"(,"pu":105)"), 3);
  EXPECT_TRUE(c.gaps().empty());

  c.ProcessLine(DepthLine(1300, 4, 111, 115, R"(,"pu":999)"), 4);
  ASSERT_EQ(c.gaps().size(), 1u);
  EXPECT_NE(c.gaps().front().reason.find("futures"), std::string::npos);
}

TEST(Converter, ASnapshotEndsTheDirtyIntervalAndCountsAsAResync) {
  Converter c(Options());
  c.ProcessLine(SnapshotLine(1000, 100), 1);
  c.ProcessLine(DepthLine(1100, 2, 500, 505), 2);  // gap -> dirty
  ASSERT_TRUE(c.dirty());
  c.ProcessLine(SnapshotLine(2000, 600), 3);
  EXPECT_FALSE(c.dirty());
  EXPECT_EQ(c.stats().resyncs, 1u);
}

TEST(Converter, CountsMalformedLinesWithoutLosingTheFile) {
  // A truncated final line after a crash must not cost the whole hour of data.
  Converter c(Options());
  c.ProcessLine(SnapshotLine(1000, 100), 1);
  EXPECT_FALSE(c.ProcessLine(R"({"t":1,"c":"depth","s":"BTCUSDT","d":{"E":)", 2));
  c.ProcessLine(DepthLine(1100, 2, 101, 105), 3);
  EXPECT_EQ(c.stats().lines_malformed, 1u);
  EXPECT_GT(c.stats().events_emitted, 0u);
}

TEST(Converter, IgnoresOtherSymbols) {
  Converter c(Options());
  c.ProcessLine(R"({"t":1,"c":"depth","s":"ETHUSDT","d":{"E":1,"U":1,"u":2,"b":[],"a":[]}})", 1);
  EXPECT_EQ(c.stats().lines_wrong_symbol, 1u);
  EXPECT_EQ(c.stats().events_emitted, 0u);
}

TEST(Converter, SortsTradesBeforeDepthAtEqualTimestamps) {
  // A level decrease may only be treated as a cancellation once the trades that
  // explain it have been applied (§3.7).
  Converter c(Options());
  c.ProcessLine(SnapshotLine(1000, 100), 1);
  // Depth first in the file, trade second, both stamped at event time 5000 ms.
  c.ProcessLine(DepthLine(1100, 5000, 101, 105), 2);
  c.ProcessLine(
      R"({"t":1200,"c":"aggTrade","s":"BTCUSDT","d":{"E":5000,"T":5000,"a":1,"p":"100.00",)"
      R"("q":"1.0","m":true}})",
      3);

  const std::vector<Event> events = c.Finish();
  int trade_index = -1;
  int depth_index = -1;
  for (std::size_t i = 0; i < events.size(); ++i) {
    if (events[i].exch_ts_us != 5'000'000) {
      continue;
    }
    if (events[i].Type() == EventType::kTrade && trade_index < 0) {
      trade_index = static_cast<int>(i);
    }
    if (events[i].Type() == EventType::kDepth && depth_index < 0) {
      depth_index = static_cast<int>(i);
    }
  }
  ASSERT_GE(trade_index, 0);
  ASSERT_GE(depth_index, 0);
  EXPECT_LT(trade_index, depth_index);
}

TEST(Converter, OutputIsSortedByTimestamp) {
  Converter c(Options());
  c.ProcessLine(SnapshotLine(1000, 100), 1);
  c.ProcessLine(DepthLine(1100, 9000, 101, 105), 2);
  c.ProcessLine(DepthLine(1200, 3000, 106, 110), 3);  // out of order in the file
  const std::vector<Event> events = c.Finish();
  for (std::size_t i = 1; i < events.size(); ++i) {
    EXPECT_LE(events[i - 1].exch_ts_us, events[i].exch_ts_us);
  }
}

// ---------------------------------------------------------------------------
// Binary I/O round trip (Phase 1 gate)
// ---------------------------------------------------------------------------
TEST(EventIo, RoundTripsAnEventArrayByteForByte) {
  const Instrument inst = testing::TestInstrument();
  const std::vector<Event> original =
      testing::SyntheticSession(inst, 3, 1'600'000'000'000'000LL, 20 * kUsPerSecond, 30);
  ASSERT_FALSE(original.empty());

  const std::string path = testing::WriteTempEvents("roundtrip", original);
  const std::vector<Event> restored = ReadAllEvents(path);
  ASSERT_EQ(restored.size(), original.size());
  for (std::size_t i = 0; i < original.size(); ++i) {
    EXPECT_EQ(original[i], restored[i]) << "record " << i;
  }
}

TEST(EventIo, HeaderRecordsCountAndTimeRange) {
  const Instrument inst = testing::TestInstrument();
  const std::vector<Event> events = testing::SimpleOpeningBook(inst);
  const std::string path = testing::WriteTempEvents("header", events);

  EventReader reader(path);
  EXPECT_EQ(reader.header().record_count, events.size());
  EXPECT_EQ(reader.header().record_size, kEventSize);
  EXPECT_EQ(reader.header().version, kEventFileVersion);
  EXPECT_EQ(reader.header().first_ts_us, events.front().exch_ts_us);
  EXPECT_EQ(reader.header().last_ts_us, events.back().exch_ts_us);
}

TEST(EventIo, RejectsFilesThatAreNotOurs) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "lob_sim_tests";
  std::filesystem::create_directories(dir);
  const std::string path = (dir / "not_lob.bin").string();
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const std::string junk(64, 'x');
    out.write(junk.data(), static_cast<std::streamsize>(junk.size()));
  }
  EXPECT_THROW(EventReader{path}, std::runtime_error);
}

TEST(EventIo, BlockReadingCoversEveryRecord) {
  const Instrument inst = testing::TestInstrument();
  const std::vector<Event> events =
      testing::SyntheticSession(inst, 9, 1'600'000'000'000'000LL, 30 * kUsPerSecond, 50);
  const std::string path = testing::WriteTempEvents("blocks", events);

  EventReader reader(path);
  std::size_t total = 0;
  const Event* block = nullptr;
  std::size_t n = 0;
  while ((n = reader.NextBlock(block)) > 0) {
    ASSERT_NE(block, nullptr);
    for (std::size_t i = 0; i < n; ++i) {
      EXPECT_EQ(block[i], events[total + i]);
    }
    total += n;
  }
  EXPECT_EQ(total, events.size());
}

}  // namespace
}  // namespace lob
