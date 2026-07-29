// Deterministic CSV output.
//
// The determinism test (CLAUDE.md Phase 3 gate) byte-compares two full runs, so
// every number written here must render identically every time.  That rules out
// operator<< with default precision (locale- and platform-sensitive) and
// printf("%g").  Doubles go through a fixed-precision formatter; anything that
// can be an integer is written as one.
//
// Column schemas are fixed by master plan §4.8 and defined in
// analytics/recorders.hpp -- this class only guarantees the mechanics.
#pragma once

#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace lob {

class CsvWriter {
 public:
  CsvWriter() = default;
  CsvWriter(const std::string& path, std::initializer_list<std::string_view> header);
  ~CsvWriter();

  CsvWriter(const CsvWriter&) = delete;
  CsvWriter& operator=(const CsvWriter&) = delete;
  // std::ofstream's move operations are not noexcept, so these are not either.
  CsvWriter(CsvWriter&&) = default;
  CsvWriter& operator=(CsvWriter&&) = default;

  void Open(const std::string& path, std::initializer_list<std::string_view> header);
  [[nodiscard]] bool is_open() const { return out_.is_open(); }
  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] std::uint64_t rows() const { return rows_; }

  // --- one field at a time -------------------------------------------------
  CsvWriter& Field(std::string_view v);
  CsvWriter& Field(const char* v) { return Field(std::string_view(v)); }
  CsvWriter& Field(std::int64_t v);
  CsvWriter& Field(std::int32_t v) { return Field(static_cast<std::int64_t>(v)); }
  CsvWriter& Field(std::uint64_t v);
  CsvWriter& Field(bool v) { return Field(std::string_view(v ? "1" : "0")); }
  // `precision` decimal places, fixed notation, half-away-from-zero rounding,
  // "nan"/"inf" spelled explicitly so a bad number is visible instead of blank.
  CsvWriter& Field(double v, int precision = 10);
  void EndRow();

  // --- whole row in one call ----------------------------------------------
  template <typename... Ts>
  void Row(Ts&&... vs) {
    (Field(std::forward<Ts>(vs)), ...);
    EndRow();
  }

  void Flush();
  void Close();

  // Formats a double the way Field() does.  Exposed for tests and for building
  // manifest strings that must match the CSVs byte for byte.
  static std::string FormatDouble(double v, int precision);

 private:
  void WriteSeparatorIfNeeded();

  std::ofstream out_;
  std::string path_;
  std::string buf_;
  std::size_t columns_ = 0;
  std::size_t field_index_ = 0;
  std::uint64_t rows_ = 0;
};

}  // namespace lob
