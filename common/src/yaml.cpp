#include <lob/yaml.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>

#include <lob/json.hpp>

namespace lob::yaml {
namespace {

struct Line {
  int indent = 0;
  std::string_view text;  // comment-stripped, right-trimmed content
  int number = 0;         // 1-based, for error messages
};

std::string_view TrimRight(std::string_view s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

std::string_view TrimLeft(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  return s;
}

std::string_view Trim(std::string_view s) { return TrimLeft(TrimRight(s)); }

// Removes a trailing "# comment", honouring quotes.  A '#' only starts a
// comment when it is at the start of the content or preceded by whitespace,
// which is the YAML rule and keeps values such as "colour: #fff" usable when
// quoted.
std::string_view StripComment(std::string_view s) {
  char quote = '\0';
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (quote != '\0') {
      if (c == quote) {
        quote = '\0';
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      continue;
    }
    if (c == '#' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
      return s.substr(0, i);
    }
  }
  return s;
}

// Splits "key: value" at the first top-level ':' that is followed by
// whitespace or end-of-content, so flow values such as {a: 1} and timestamps
// such as 12:30:00 on the value side are not mistaken for the separator.
bool SplitKeyValue(std::string_view s, std::string_view& key, std::string_view& value) {
  char quote = '\0';
  int depth = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (quote != '\0') {
      if (c == quote) {
        quote = '\0';
      }
      continue;
    }
    switch (c) {
      case '"':
      case '\'':
        quote = c;
        break;
      case '[':
      case '{':
        ++depth;
        break;
      case ']':
      case '}':
        --depth;
        break;
      case ':':
        if (depth == 0 && (i + 1 == s.size() || s[i + 1] == ' ' || s[i + 1] == '\t')) {
          key = Trim(s.substr(0, i));
          value = Trim(s.substr(i + 1));
          return true;
        }
        break;
      default:
        break;
    }
  }
  return false;
}

std::string Unquote(std::string_view s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    // Double-quoted: honour the common backslash escapes.
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 1; i + 1 < s.size(); ++i) {
      if (s[i] == '\\' && i + 2 < s.size()) {
        switch (s[++i]) {
          case 'n': out.push_back('\n'); break;
          case 't': out.push_back('\t'); break;
          case 'r': out.push_back('\r'); break;
          case '\\': out.push_back('\\'); break;
          case '"': out.push_back('"'); break;
          default: out.push_back(s[i]); break;
        }
      } else {
        out.push_back(s[i]);
      }
    }
    return out;
  }
  if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
    // Single-quoted: only '' is an escape.
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 1; i + 1 < s.size(); ++i) {
      out.push_back(s[i]);
      if (s[i] == '\'' && i + 2 < s.size() && s[i + 1] == '\'') {
        ++i;
      }
    }
    return out;
  }
  return std::string(s);
}

// Splits a flow collection body on top-level commas.
std::vector<std::string_view> SplitFlow(std::string_view body, int line) {
  std::vector<std::string_view> parts;
  char quote = '\0';
  int depth = 0;
  std::size_t start = 0;
  for (std::size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (quote != '\0') {
      if (c == quote) {
        quote = '\0';
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '[' || c == '{') {
      ++depth;
    } else if (c == ']' || c == '}') {
      --depth;
      if (depth < 0) {
        throw ParseError("unbalanced flow collection", line);
      }
    } else if (c == ',' && depth == 0) {
      parts.push_back(Trim(body.substr(start, i - start)));
      start = i + 1;
    }
  }
  if (depth != 0 || quote != '\0') {
    throw ParseError("unterminated flow collection", line);
  }
  const std::string_view tail = Trim(body.substr(start));
  if (!tail.empty()) {
    parts.push_back(tail);
  }
  return parts;
}

Node ParseFlow(std::string_view s, int line);

Node ParseScalarOrFlow(std::string_view s, int line) {
  s = Trim(s);
  if (s.empty()) {
    return Node();
  }
  if (s.front() == '[' || s.front() == '{') {
    return ParseFlow(s, line);
  }
  if (s == "~" || s == "null" || s == "Null" || s == "NULL") {
    return Node();
  }
  if (s.front() == '|' || s.front() == '>') {
    throw ParseError("block scalars (| and >) are not supported by this config reader", line);
  }
  if (s.front() == '&' || s.front() == '*' || s.front() == '!') {
    throw ParseError("anchors, aliases and tags are not supported by this config reader", line);
  }
  return Node::Scalar(Unquote(s));
}

Node ParseFlow(std::string_view s, int line) {
  if (s.size() < 2) {
    throw ParseError("malformed flow collection", line);
  }
  if (s.front() == '[') {
    if (s.back() != ']') {
      throw ParseError("flow sequence is not closed on the same line", line);
    }
    Node seq = Node::Sequence();
    for (std::string_view part : SplitFlow(s.substr(1, s.size() - 2), line)) {
      seq.Push(ParseScalarOrFlow(part, line));
    }
    return seq;
  }
  if (s.back() != '}') {
    throw ParseError("flow mapping is not closed on the same line", line);
  }
  Node map = Node::Map();
  for (std::string_view part : SplitFlow(s.substr(1, s.size() - 2), line)) {
    std::string_view k;
    std::string_view v;
    if (!SplitKeyValue(part, k, v)) {
      throw ParseError("flow mapping entry is missing ': '", line);
    }
    map.Insert(Unquote(k), ParseScalarOrFlow(v, line));
  }
  return map;
}

// ---------------------------------------------------------------------------
// Block parser
// ---------------------------------------------------------------------------
class BlockParser {
 public:
  explicit BlockParser(std::vector<Line> lines) : lines_(std::move(lines)) {}

  Node ParseDocument() {
    if (lines_.empty()) {
      return Node();
    }
    Node root = ParseBlock(lines_.front().indent);
    if (i_ < lines_.size()) {
      throw ParseError("unexpected dedent / trailing content", lines_[i_].number);
    }
    return root;
  }

 private:
  Node ParseBlock(int indent) {
    if (i_ >= lines_.size() || lines_[i_].indent < indent) {
      return Node();
    }
    if (lines_[i_].text.starts_with("- ") || lines_[i_].text == "-") {
      return ParseSequence(indent);
    }
    return ParseMap(indent);
  }

  Node ParseSequence(int indent) {
    Node seq = Node::Sequence();
    while (i_ < lines_.size() && lines_[i_].indent == indent &&
           (lines_[i_].text.starts_with("- ") || lines_[i_].text == "-")) {
      const Line line = lines_[i_];
      std::string_view rest = Trim(line.text.substr(1));
      ++i_;
      if (rest.empty()) {
        seq.Push(ParseChildBlock(indent, line.number));
        continue;
      }
      // "- key: value" starts a nested mapping whose first key sits at
      // indent + 2 (the width of the "- " marker).
      std::string_view k;
      std::string_view v;
      if (SplitKeyValue(rest, k, v) && rest.front() != '{' && rest.front() != '[') {
        Node map = Node::Map();
        map.Insert(Unquote(k), v.empty() ? ParseChildBlock(indent, line.number)
                                         : ParseScalarOrFlow(v, line.number));
        while (i_ < lines_.size() && lines_[i_].indent == indent + 2) {
          std::string_view k2;
          std::string_view v2;
          if (!SplitKeyValue(lines_[i_].text, k2, v2)) {
            throw ParseError("expected 'key: value' inside sequence item", lines_[i_].number);
          }
          const int n2 = lines_[i_].number;
          ++i_;
          map.Insert(Unquote(k2),
                     v2.empty() ? ParseChildBlock(indent + 2, n2) : ParseScalarOrFlow(v2, n2));
        }
        seq.Push(std::move(map));
        continue;
      }
      seq.Push(ParseScalarOrFlow(rest, line.number));
    }
    return seq;
  }

  Node ParseMap(int indent) {
    Node map = Node::Map();
    while (i_ < lines_.size() && lines_[i_].indent == indent) {
      const Line line = lines_[i_];
      if (line.text.starts_with("- ")) {
        throw ParseError("sequence item where a mapping key was expected", line.number);
      }
      std::string_view k;
      std::string_view v;
      if (!SplitKeyValue(line.text, k, v)) {
        throw ParseError("expected 'key: value'", line.number);
      }
      ++i_;
      if (v.empty()) {
        map.Insert(Unquote(k), ParseChildBlock(indent, line.number));
      } else {
        map.Insert(Unquote(k), ParseScalarOrFlow(v, line.number));
      }
    }
    if (i_ < lines_.size() && lines_[i_].indent > indent) {
      throw ParseError("unexpected indent", lines_[i_].number);
    }
    return map;
  }

  // Parses whatever is nested under the line just consumed.  An empty value
  // with no deeper lines is a null scalar (a legal YAML "key:").
  Node ParseChildBlock(int parent_indent, int /*parent_line*/) {
    if (i_ >= lines_.size() || lines_[i_].indent <= parent_indent) {
      return Node();
    }
    return ParseBlock(lines_[i_].indent);
  }

  std::vector<Line> lines_;
  std::size_t i_ = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// Node
// ---------------------------------------------------------------------------
Node Node::Scalar(std::string v) {
  Node n;
  n.kind_ = Kind::kScalar;
  n.scalar_ = std::move(v);
  return n;
}

Node Node::Sequence() {
  Node n;
  n.kind_ = Kind::kSequence;
  return n;
}

Node Node::Map() {
  Node n;
  n.kind_ = Kind::kMap;
  return n;
}

namespace {
const Node& NullNode() {
  static const Node kNull;
  return kNull;
}
}  // namespace

bool Node::Has(std::string_view key) const {
  return std::any_of(map_.begin(), map_.end(),
                     [key](const auto& kv) { return kv.first == key; });
}

const Node& Node::operator[](std::string_view key) const {
  for (const auto& kv : map_) {
    if (kv.first == key) {
      return kv.second;
    }
  }
  return NullNode();
}

void Node::Insert(std::string key, Node value) {
  for (auto& kv : map_) {
    if (kv.first == key) {
      kv.second = std::move(value);
      return;
    }
  }
  kind_ = Kind::kMap;
  map_.emplace_back(std::move(key), std::move(value));
}

std::size_t Node::size() const {
  if (kind_ == Kind::kSequence) {
    return seq_.size();
  }
  if (kind_ == Kind::kMap) {
    return map_.size();
  }
  return 0;
}

const Node& Node::operator[](std::size_t i) const {
  if (kind_ != Kind::kSequence || i >= seq_.size()) {
    return NullNode();
  }
  return seq_[i];
}

void Node::Push(Node value) {
  kind_ = Kind::kSequence;
  seq_.push_back(std::move(value));
}

namespace {
[[noreturn]] void Missing(std::string_view context, std::string_view what) {
  throw ParseError("config key " + std::string(context.empty() ? "<anonymous>" : context) +
                       " is missing or not a " + std::string(what),
                   0);
}
}  // namespace

std::string Node::AsString(std::string_view context) const {
  if (!IsScalar()) {
    Missing(context, "scalar");
  }
  return scalar_;
}

std::int64_t Node::AsInt(std::string_view context) const {
  if (!IsScalar()) {
    Missing(context, "scalar");
  }
  std::int64_t v = 0;
  if (json::ParseInt(scalar_, v)) {
    return v;
  }
  // Tolerate integral values written in float form (1.0e5, 42.0), which YAML
  // authors do routinely; reject anything with a real fractional part.
  double d = 0.0;
  if (json::ParseDouble(scalar_, d) && d == static_cast<double>(static_cast<std::int64_t>(d))) {
    return static_cast<std::int64_t>(d);
  }
  throw ParseError("config key " + std::string(context) + " = '" + scalar_ + "' is not an integer",
                   0);
}

double Node::AsDouble(std::string_view context) const {
  if (!IsScalar()) {
    Missing(context, "scalar");
  }
  double v = 0.0;
  if (!json::ParseDouble(scalar_, v)) {
    throw ParseError("config key " + std::string(context) + " = '" + scalar_ + "' is not a number",
                     0);
  }
  return v;
}

bool Node::AsBool(std::string_view context) const {
  if (!IsScalar()) {
    Missing(context, "scalar");
  }
  std::string lowered;
  lowered.reserve(scalar_.size());
  for (char c : scalar_) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lowered == "true" || lowered == "yes" || lowered == "on" || lowered == "1") {
    return true;
  }
  if (lowered == "false" || lowered == "no" || lowered == "off" || lowered == "0") {
    return false;
  }
  throw ParseError("config key " + std::string(context) + " = '" + scalar_ + "' is not a boolean",
                   0);
}

std::string Node::AsStringOr(std::string_view fallback) const {
  return IsScalar() ? scalar_ : std::string(fallback);
}

std::int64_t Node::AsIntOr(std::int64_t fallback) const {
  if (!IsScalar()) {
    return fallback;
  }
  std::int64_t v = 0;
  if (json::ParseInt(scalar_, v)) {
    return v;
  }
  double d = 0.0;
  if (json::ParseDouble(scalar_, d) && d == static_cast<double>(static_cast<std::int64_t>(d))) {
    return static_cast<std::int64_t>(d);
  }
  return fallback;
}

double Node::AsDoubleOr(double fallback) const {
  if (!IsScalar()) {
    return fallback;
  }
  double v = 0.0;
  return json::ParseDouble(scalar_, v) ? v : fallback;
}

bool Node::AsBoolOr(bool fallback) const {
  if (!IsScalar()) {
    return fallback;
  }
  try {
    return AsBool();
  } catch (const ParseError&) {
    return fallback;
  }
}

std::string Node::Dump(int indent) const {
  const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
  std::ostringstream os;
  switch (kind_) {
    case Kind::kNull:
      os << "~";
      break;
    case Kind::kScalar:
      os << scalar_;
      break;
    case Kind::kSequence:
      for (const Node& n : seq_) {
        os << "\n" << pad << "- " << n.Dump(indent + 1);
      }
      break;
    case Kind::kMap:
      for (const auto& [k, v] : map_) {
        os << "\n" << pad << k << ":";
        if (v.IsScalar() || v.IsNull()) {
          os << " " << v.Dump(indent + 1);
        } else {
          os << v.Dump(indent + 1);
        }
      }
      break;
  }
  return os.str();
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------
Node Parse(std::string_view text) {
  std::vector<Line> lines;
  int number = 0;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const std::size_t nl = text.find('\n', pos);
    std::string_view raw =
        text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    ++number;

    std::string_view content = TrimRight(StripComment(raw));
    if (!content.empty() && content != "---" && content != "...") {
      if (content.find('\t') < content.find_first_not_of(" \t")) {
        throw ParseError("tabs may not be used for indentation", number);
      }
      Line line;
      line.number = number;
      line.indent = static_cast<int>(content.size() - TrimLeft(content).size());
      line.text = TrimLeft(content);
      lines.push_back(line);
    }
    if (nl == std::string_view::npos) {
      break;
    }
    pos = nl + 1;
  }
  BlockParser parser(std::move(lines));
  return parser.ParseDocument();
}

Node ParseFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw ParseError("cannot open config file: " + path, 0);
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  const std::string text = buf.str();
  try {
    return Parse(text);
  } catch (const ParseError& e) {
    throw ParseError(path + ":" + std::to_string(e.line()) + ": " + e.what(), e.line());
  }
}

}  // namespace lob::yaml
