// A deliberately small YAML reader covering exactly the subset the experiment
// configs use (master plan Appendix A):
//
//   * block mappings nested by indentation
//   * block sequences ("- item")
//   * flow sequences  ("[a, b, c]")
//   * flow mappings   ("{k: v, j: w}")
//   * plain / single- / double-quoted scalars, "#" comments, "---" doc marker
//
// Anchors, aliases, multi-document streams, block scalars (| and >), tags and
// complex keys are NOT supported and are reported as errors rather than
// silently mis-parsed.  A config format that fails loudly beats a dependency.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lob::yaml {

class ParseError : public std::runtime_error {
 public:
  ParseError(const std::string& what, int line) : std::runtime_error(what), line_(line) {}
  [[nodiscard]] int line() const { return line_; }

 private:
  int line_;
};

class Node {
 public:
  enum class Kind : std::uint8_t { kNull, kScalar, kSequence, kMap };

  Node() = default;
  static Node Scalar(std::string v);
  static Node Sequence();
  static Node Map();

  [[nodiscard]] Kind kind() const { return kind_; }
  [[nodiscard]] bool IsNull() const { return kind_ == Kind::kNull; }
  [[nodiscard]] bool IsScalar() const { return kind_ == Kind::kScalar; }
  [[nodiscard]] bool IsSequence() const { return kind_ == Kind::kSequence; }
  [[nodiscard]] bool IsMap() const { return kind_ == Kind::kMap; }

  // --- map access ----------------------------------------------------------
  [[nodiscard]] bool Has(std::string_view key) const;
  // Returns a shared static null node when the key is absent, so chained
  // lookups such as cfg["strategy"]["gamma"] never dereference nothing.
  [[nodiscard]] const Node& operator[](std::string_view key) const;
  [[nodiscard]] const std::vector<std::pair<std::string, Node>>& items() const { return map_; }
  void Insert(std::string key, Node value);

  // --- sequence access -----------------------------------------------------
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] const Node& operator[](std::size_t i) const;
  [[nodiscard]] const std::vector<Node>& elements() const { return seq_; }
  void Push(Node value);

  // --- scalar access -------------------------------------------------------
  // The typed getters throw ParseError when the node is missing or the scalar
  // does not convert; the `Or` variants return the fallback instead.  Configs
  // should use the throwing form for required keys so a typo is a hard failure
  // (master plan "no unexplained magic numbers").
  [[nodiscard]] const std::string& raw() const { return scalar_; }
  [[nodiscard]] std::string AsString(std::string_view context = {}) const;
  [[nodiscard]] std::int64_t AsInt(std::string_view context = {}) const;
  [[nodiscard]] double AsDouble(std::string_view context = {}) const;
  [[nodiscard]] bool AsBool(std::string_view context = {}) const;

  [[nodiscard]] std::string AsStringOr(std::string_view fallback) const;
  [[nodiscard]] std::int64_t AsIntOr(std::int64_t fallback) const;
  [[nodiscard]] double AsDoubleOr(double fallback) const;
  [[nodiscard]] bool AsBoolOr(bool fallback) const;

  // Serialises back to a canonical, deterministic YAML string.  Used to embed
  // the resolved config in run manifests so every result is traceable.
  [[nodiscard]] std::string Dump(int indent = 0) const;

 private:
  Kind kind_ = Kind::kNull;
  std::string scalar_;
  std::vector<Node> seq_;
  std::vector<std::pair<std::string, Node>> map_;
};

// Parses a complete document.  Throws ParseError with a line number.
Node Parse(std::string_view text);
Node ParseFile(const std::string& path);

}  // namespace lob::yaml
