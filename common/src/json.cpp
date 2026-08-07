#include <lob/json.hpp>

#include <charconv>
#include <cstdlib>

namespace lob::json {
namespace {

constexpr bool IsWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

constexpr bool IsNumberStart(char c) { return c == '-' || (c >= '0' && c <= '9'); }

// Appends the UTF-8 encoding of `cp` to `out`.
void AppendUtf8(std::uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

bool HexQuad(std::string_view s, std::size_t at, std::uint32_t& out) {
  if (at + 4 > s.size()) {
    return false;
  }
  std::uint32_t v = 0;
  for (std::size_t k = 0; k < 4; ++k) {
    const char c = s[at + k];
    v <<= 4;
    if (c >= '0' && c <= '9') {
      v |= static_cast<std::uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      v |= static_cast<std::uint32_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      v |= static_cast<std::uint32_t>(c - 'A' + 10);
    } else {
      return false;
    }
  }
  out = v;
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------
bool ParseDouble(std::string_view text, double& out) {
  if (text.empty()) {
    return false;
  }
  const char* first = text.data();
  const char* last = text.data() + text.size();
#if defined(__cpp_lib_to_chars) || defined(_MSC_VER) || (defined(__GNUC__) && __GNUC__ >= 11)
  const auto res = std::from_chars(first, last, out);
  return res.ec == std::errc{} && res.ptr == last;
#else
  // Fallback for toolchains without floating-point from_chars.  strtod is
  // locale-sensitive in principle; the project sets the C locale at start-up.
  std::string buf(text);
  char* end = nullptr;
  out = std::strtod(buf.c_str(), &end);
  return end != nullptr && *end == '\0' && end != buf.c_str();
#endif
}

bool ParseFixedPoint(std::string_view text, int decimals, std::int64_t& out) {
  if (text.empty() || decimals < 0 || decimals > 18) {
    return false;
  }
  std::size_t i = 0;
  bool negative = false;
  if (text[i] == '+' || text[i] == '-') {
    negative = (text[i] == '-');
    ++i;
  }

  std::int64_t value = 0;
  bool any_digit = false;
  // Integer part.
  for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
    // Guard against a hostile or corrupt field overflowing silently.
    if (value > (std::numeric_limits<std::int64_t>::max() - 9) / 10) {
      return false;
    }
    value = value * 10 + (text[i] - '0');
    any_digit = true;
  }

  int consumed = 0;
  bool round_up = false;
  if (i < text.size() && text[i] == '.') {
    ++i;
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
      const int digit = text[i] - '0';
      any_digit = true;
      if (consumed < decimals) {
        if (value > (std::numeric_limits<std::int64_t>::max() - 9) / 10) {
          return false;
        }
        value = value * 10 + digit;
        ++consumed;
      } else if (consumed == decimals) {
        // First discarded place decides the rounding; half away from zero.
        round_up = digit >= 5;
        ++consumed;
      }
    }
  }
  // Anything left over (an exponent, a stray character) means this is not a
  // plain decimal and the caller should use the general path.
  if (i != text.size() || !any_digit) {
    return false;
  }
  // Pad when the input carried fewer places than the grid needs.
  for (; consumed < decimals; ++consumed) {
    if (value > (std::numeric_limits<std::int64_t>::max() - 9) / 10) {
      return false;
    }
    value *= 10;
  }
  if (round_up) {
    ++value;
  }
  out = negative ? -value : value;
  return true;
}

bool ParseInt(std::string_view text, std::int64_t& out) {
  if (text.empty()) {
    return false;
  }
  const char* first = text.data();
  const char* last = text.data() + text.size();
  const auto res = std::from_chars(first, last, out);
  return res.ec == std::errc{} && res.ptr == last;
}

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------
void Reader::Fail(std::string_view why) {
  if (ok_) {
    ok_ = false;
    error_ = why;
  }
}

void Reader::SkipWs() {
  while (i_ < s_.size() && IsWs(s_[i_])) {
    ++i_;
  }
}

char Reader::Peek() {
  SkipWs();
  return i_ < s_.size() ? s_[i_] : '\0';
}

bool Reader::AtEnd() {
  SkipWs();
  return i_ >= s_.size();
}

bool Reader::Expect(char c) {
  SkipWs();
  if (i_ >= s_.size() || s_[i_] != c) {
    Fail("expected character");
    return false;
  }
  ++i_;
  return true;
}

bool Reader::EnterObject() {
  if (!ok_) {
    return false;
  }
  if (!Expect('{')) {
    Fail("expected '{'");
    return false;
  }
  fresh_container_ = true;
  return true;
}

bool Reader::EnterArray() {
  if (!ok_) {
    return false;
  }
  if (!Expect('[')) {
    Fail("expected '['");
    return false;
  }
  fresh_container_ = true;
  return true;
}

bool Reader::NextMember(std::string_view& key) {
  if (!ok_) {
    return false;
  }
  SkipWs();
  if (i_ >= s_.size()) {
    Fail("unterminated object");
    return false;
  }
  if (s_[i_] == '}') {
    ++i_;
    fresh_container_ = false;
    return false;
  }
  if (fresh_container_) {
    fresh_container_ = false;
  } else if (s_[i_] == ',') {
    ++i_;
  } else {
    Fail("expected ',' or '}' in object");
    return false;
  }
  SkipWs();
  if (i_ < s_.size() && s_[i_] == '}') {  // trailing comma
    Fail("trailing comma in object");
    return false;
  }
  if (!ScanStringSpan(key, nullptr)) {
    Fail("expected object key");
    return false;
  }
  if (!Expect(':')) {
    Fail("expected ':' after object key");
    return false;
  }
  return true;
}

bool Reader::NextElement() {
  if (!ok_) {
    return false;
  }
  SkipWs();
  if (i_ >= s_.size()) {
    Fail("unterminated array");
    return false;
  }
  if (s_[i_] == ']') {
    ++i_;
    fresh_container_ = false;
    return false;
  }
  if (fresh_container_) {
    fresh_container_ = false;
  } else if (s_[i_] == ',') {
    ++i_;
  } else {
    Fail("expected ',' or ']' in array");
    return false;
  }
  SkipWs();
  if (i_ < s_.size() && s_[i_] == ']') {  // trailing comma
    Fail("trailing comma in array");
    return false;
  }
  return true;
}

bool Reader::ScanStringSpan(std::string_view& out, bool* had_escapes) {
  SkipWs();
  if (i_ >= s_.size() || s_[i_] != '"') {
    return false;
  }
  const std::size_t start = ++i_;
  bool escapes = false;
  while (i_ < s_.size()) {
    const char c = s_[i_];
    if (c == '\\') {
      escapes = true;
      i_ += 2;  // skip the escape introducer and one payload char
      continue;
    }
    if (c == '"') {
      out = s_.substr(start, i_ - start);
      ++i_;
      if (had_escapes != nullptr) {
        *had_escapes = escapes;
      }
      return true;
    }
    ++i_;
  }
  return false;
}

bool Reader::ReadStringRaw(std::string_view& out, bool* had_escapes) {
  if (!ok_) {
    return false;
  }
  if (!ScanStringSpan(out, had_escapes)) {
    Fail("expected string");
    return false;
  }
  return true;
}

bool Reader::ReadStringCopy(std::string& out) {
  std::string_view raw;
  bool escaped = false;
  if (!ReadStringRaw(raw, &escaped)) {
    return false;
  }
  out.clear();
  if (!escaped) {
    out.assign(raw);
    return true;
  }
  out.reserve(raw.size());
  for (std::size_t k = 0; k < raw.size(); ++k) {
    const char c = raw[k];
    if (c != '\\') {
      out.push_back(c);
      continue;
    }
    if (++k >= raw.size()) {
      Fail("dangling escape in string");
      return false;
    }
    switch (raw[k]) {
      case '"':
        out.push_back('"');
        break;
      case '\\':
        out.push_back('\\');
        break;
      case '/':
        out.push_back('/');
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'u': {
        std::uint32_t cp = 0;
        if (!HexQuad(raw, k + 1, cp)) {
          Fail("bad \\u escape");
          return false;
        }
        k += 4;
        // Surrogate pair.
        if (cp >= 0xD800 && cp <= 0xDBFF && k + 6 < raw.size() && raw[k + 1] == '\\' &&
            raw[k + 2] == 'u') {
          std::uint32_t lo = 0;
          if (HexQuad(raw, k + 3, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            k += 6;
          }
        }
        AppendUtf8(cp, out);
        break;
      }
      default:
        Fail("unknown escape in string");
        return false;
    }
  }
  return true;
}

bool Reader::ReadInt(std::int64_t& out) {
  if (!ok_) {
    return false;
  }
  SkipWs();
  const std::size_t start = i_;
  if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) {
    ++i_;
  }
  while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') {
    ++i_;
  }
  if (i_ == start || !ParseInt(s_.substr(start, i_ - start), out)) {
    Fail("expected integer");
    return false;
  }
  return true;
}

bool Reader::ReadDouble(double& out) {
  if (!ok_) {
    return false;
  }
  SkipWs();
  const std::size_t start = i_;
  if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) {
    ++i_;
  }
  while (i_ < s_.size()) {
    const char c = s_[i_];
    if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
      ++i_;
    } else {
      break;
    }
  }
  if (i_ == start || !ParseDouble(s_.substr(start, i_ - start), out)) {
    Fail("expected number");
    return false;
  }
  return true;
}

bool Reader::ReadBool(bool& out) {
  if (!ok_) {
    return false;
  }
  SkipWs();
  if (s_.compare(i_, 4, "true") == 0) {
    i_ += 4;
    out = true;
    return true;
  }
  if (s_.compare(i_, 5, "false") == 0) {
    i_ += 5;
    out = false;
    return true;
  }
  Fail("expected boolean");
  return false;
}

bool Reader::ReadNull() {
  if (!ok_) {
    return false;
  }
  SkipWs();
  if (s_.compare(i_, 4, "null") == 0) {
    i_ += 4;
    return true;
  }
  Fail("expected null");
  return false;
}

bool Reader::ReadNumberLoose(double& out) {
  if (!ok_) {
    return false;
  }
  SkipWs();
  if (i_ < s_.size() && s_[i_] == '"') {
    std::string_view raw;
    if (!ReadStringRaw(raw, nullptr)) {
      return false;
    }
    if (!ParseDouble(raw, out)) {
      Fail("quoted value is not a number");
      return false;
    }
    return true;
  }
  return ReadDouble(out);
}

bool Reader::ReadFixedPoint(int decimals, std::int64_t& out) {
  if (!ok_) {
    return false;
  }
  SkipWs();
  std::string_view text;
  if (i_ < s_.size() && s_[i_] == '"') {
    if (!ReadStringRaw(text, nullptr)) {
      return false;
    }
  } else {
    const std::size_t start = i_;
    if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) {
      ++i_;
    }
    while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.')) {
      ++i_;
    }
    if (i_ == start) {
      Fail("expected a decimal number");
      return false;
    }
    text = s_.substr(start, i_ - start);
  }
  if (!ParseFixedPoint(text, decimals, out)) {
    Fail("value is not a plain decimal");
    return false;
  }
  return true;
}

bool Reader::ReadIntLoose(std::int64_t& out) {
  if (!ok_) {
    return false;
  }
  SkipWs();
  if (i_ < s_.size() && s_[i_] == '"') {
    std::string_view raw;
    if (!ReadStringRaw(raw, nullptr)) {
      return false;
    }
    if (!ParseInt(raw, out)) {
      Fail("quoted value is not an integer");
      return false;
    }
    return true;
  }
  return ReadInt(out);
}

bool Reader::SkipValueSpan(std::string_view& span) {
  if (!ok_) {
    return false;
  }
  SkipWs();
  if (i_ >= s_.size()) {
    Fail("expected a value");
    return false;
  }
  const std::size_t start = i_;
  const char c = s_[i_];

  if (c == '{' || c == '[') {
    // Bracket matching.  Correctness rests on one thing: brace and bracket
    // characters inside string literals must not be counted, so the scan
    // tracks string state and steps over backslash escapes.
    int depth = 0;
    bool in_string = false;
    while (i_ < s_.size()) {
      const char ch = s_[i_];
      if (in_string) {
        if (ch == '\\') {
          i_ += 2;
          continue;
        }
        if (ch == '"') {
          in_string = false;
        }
        ++i_;
        continue;
      }
      if (ch == '"') {
        in_string = true;
        ++i_;
        continue;
      }
      if (ch == '{' || ch == '[') {
        ++depth;
        ++i_;
        continue;
      }
      if (ch == '}' || ch == ']') {
        --depth;
        ++i_;
        if (depth == 0) {
          span = s_.substr(start, i_ - start);
          return true;
        }
        if (depth < 0) {
          Fail("unbalanced bracket");
          return false;
        }
        continue;
      }
      ++i_;
    }
    Fail("unterminated container");
    return false;
  }

  // Scalars are cheap enough that the general skipper is fine.
  if (!SkipValue()) {
    return false;
  }
  span = s_.substr(start, i_ - start);
  return true;
}

bool Reader::PeekNextKeyIs(std::string_view key) {
  if (!ok_) {
    return false;
  }
  const std::size_t saved_i = i_;
  const bool saved_fresh = fresh_container_;
  std::string_view found;
  const bool matched = NextMember(found) && found == key;
  // Restore the cursor whatever happened; this is a pure lookahead.
  i_ = saved_i;
  fresh_container_ = saved_fresh;
  ok_ = true;
  error_ = {};
  return matched;
}

bool Reader::SkipValue() {
  if (!ok_) {
    return false;
  }
  SkipWs();
  if (i_ >= s_.size()) {
    Fail("expected a value");
    return false;
  }
  const char c = s_[i_];
  if (c == '"') {
    std::string_view ignored;
    return ReadStringRaw(ignored, nullptr);
  }
  if (c == 't' || c == 'f') {
    bool ignored = false;
    return ReadBool(ignored);
  }
  if (c == 'n') {
    return ReadNull();
  }
  if (IsNumberStart(c)) {
    double ignored = 0.0;
    return ReadDouble(ignored);
  }
  // Nested containers: walking one to its closing brace leaves
  // fresh_container_ == false, which is exactly the state the *enclosing*
  // container needs (it is mid-iteration, so a comma is required next).
  if (c == '{') {
    if (!EnterObject()) {
      return false;
    }
    std::string_view key;
    while (NextMember(key)) {
      if (!SkipValue()) {
        return false;
      }
    }
    return ok_;
  }
  if (c == '[') {
    if (!EnterArray()) {
      return false;
    }
    while (NextElement()) {
      if (!SkipValue()) {
        return false;
      }
    }
    return ok_;
  }
  Fail("unexpected character where a value was expected");
  return false;
}

}  // namespace lob::json
