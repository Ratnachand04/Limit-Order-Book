// A minimal, allocation-free JSON *pull* parser.
//
// Why not a DOM library: master plan Part 11 pitfall #2 -- "JSON parsing costs
// 100x the book update".  The converter is the only component that ever sees
// JSON, and it always knows the shape it expects, so a cursor that walks the
// document and hands out string_views into the caller's buffer is both faster
// and simpler than building nodes we would immediately throw away.
//
// Contract:
//   * The Reader never copies and never allocates (except read_string_copy).
//   * Errors are STICKY: after the first failure every call is a no-op that
//     returns false, so callers may chain and check once at the end.
//   * Only the JSON subset Binance emits is required, but the skipper is
//     complete so unknown fields of any shape can be stepped over.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace lob::json {

class Reader {
 public:
  explicit Reader(std::string_view text) : s_(text) {}

  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] std::string_view error() const { return error_; }
  [[nodiscard]] std::size_t pos() const { return i_; }

  // Structure -------------------------------------------------------------
  bool EnterObject();  // consumes '{'
  bool EnterArray();   // consumes '['

  // Iterates members of the object most recently entered.  Returns false and
  // consumes '}' when the object ends.  `key` points into the source buffer.
  bool NextMember(std::string_view& key);

  // Iterates elements of the array most recently entered.  Returns false and
  // consumes ']' when the array ends.
  bool NextElement();

  // Scalars ---------------------------------------------------------------
  // Returns the raw span between the quotes.  `had_escapes` tells the caller
  // whether the span still contains backslash escapes; Binance payload fields
  // (symbols, decimal prices) never do, so the fast path can ignore it.
  bool ReadStringRaw(std::string_view& out, bool* had_escapes = nullptr);
  bool ReadStringCopy(std::string& out);  // fully unescaped, allocates
  bool ReadInt(std::int64_t& out);
  bool ReadDouble(double& out);
  bool ReadBool(bool& out);
  bool ReadNull();

  // Binance sends every numeric market-data field as a JSON *string*
  // ("99999.99").  This accepts either form.
  bool ReadNumberLoose(double& out);
  bool ReadIntLoose(std::int64_t& out);

  // Reads a decimal (quoted or bare) as a fixed-point integer with `decimals`
  // implied places: "99999.99" with decimals=2 yields 9999999.
  //
  // This is the converter's hot path.  Prices and quantities live on a decimal
  // grid, so the integer the book actually wants is already sitting in the
  // digits; going via double costs a from_chars, a multiply and a llround to
  // get back to it.  Excess input places are rounded half-away-from-zero, so
  // the result matches llround(value / size) exactly.
  bool ReadFixedPoint(int decimals, std::int64_t& out);

  // Skips exactly one value of any type at the cursor.
  bool SkipValue();

  // Skips one value and hands back the span it occupied.
  //
  // For containers this uses BRACKET MATCHING rather than a recursive parse.
  // The converter only ever needs the *extent* of the recorder envelope's "d"
  // payload, not its contents, and walking it recursively meant every message
  // was parsed three times: once to find the payload, once to probe for
  // combined-stream wrapping, and once for real.  Bracket matching is a single
  // tight loop with no per-token dispatch.
  bool SkipValueSpan(std::string_view& span);

  // True when the next member key of the object being iterated equals `key`,
  // without consuming anything.  Lets a caller test for an optional wrapper
  // (such as the combined-stream {"stream":...,"data":...} form) in O(key)
  // instead of walking the whole object.
  [[nodiscard]] bool PeekNextKeyIs(std::string_view key);

  // Peeking ---------------------------------------------------------------
  [[nodiscard]] char Peek();          // '\0' at end of input
  [[nodiscard]] bool AtEnd();

  void Fail(std::string_view why);

 private:
  void SkipWs();
  bool Expect(char c);
  bool ScanStringSpan(std::string_view& out, bool* had_escapes);

  std::string_view s_;
  std::size_t i_ = 0;
  bool ok_ = true;
  // Set to true after EnterObject/EnterArray and cleared by the first
  // NextMember/NextElement, so the iterator knows not to require a comma.
  bool fresh_container_ = false;
  std::string_view error_;
};

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------
// Parses a decimal string ("0.00120000") without going through the locale.
// Returns false on malformed input.  Used by the loose readers above.
bool ParseDouble(std::string_view text, double& out);
bool ParseInt(std::string_view text, std::int64_t& out);

// "-12.345" with decimals=2 -> -1235 (half away from zero).  Returns false on
// anything that is not a plain decimal, including exponent notation, so the
// caller can fall back to the double path.
bool ParseFixedPoint(std::string_view text, int decimals, std::int64_t& out);

}  // namespace lob::json
