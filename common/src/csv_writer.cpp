#include <lob/csv_writer.hpp>

#include <charconv>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <system_error>

namespace lob {
namespace {

constexpr int kMaxPrecision = 17;

bool NeedsQuoting(std::string_view v) {
  return v.find_first_of(",\"\n\r") != std::string_view::npos;
}

}  // namespace

CsvWriter::CsvWriter(const std::string& path, std::initializer_list<std::string_view> header) {
  Open(path, header);
}

CsvWriter::~CsvWriter() {
  if (out_.is_open()) {
    Close();
  }
}

void CsvWriter::Open(const std::string& path, std::initializer_list<std::string_view> header) {
  path_ = path;
  const std::filesystem::path fs_path(path);
  if (fs_path.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(fs_path.parent_path(), ec);
  }
  // Binary mode: the determinism test byte-compares outputs, and text mode
  // would emit CRLF on Windows and LF on Linux for the same run.
  out_.open(path, std::ios::binary | std::ios::trunc);
  if (!out_) {
    throw std::runtime_error("CsvWriter: cannot open " + path + " for writing");
  }
  buf_.reserve(1 << 16);
  columns_ = header.size();
  field_index_ = 0;
  rows_ = 0;

  bool first = true;
  for (std::string_view h : header) {
    if (!first) {
      buf_.push_back(',');
    }
    first = false;
    buf_.append(h);
  }
  buf_.push_back('\n');
}

void CsvWriter::WriteSeparatorIfNeeded() {
  if (field_index_ > 0) {
    buf_.push_back(',');
  }
  ++field_index_;
}

CsvWriter& CsvWriter::Field(std::string_view v) {
  WriteSeparatorIfNeeded();
  if (NeedsQuoting(v)) {
    buf_.push_back('"');
    for (char c : v) {
      if (c == '"') {
        buf_.push_back('"');
      }
      buf_.push_back(c);
    }
    buf_.push_back('"');
  } else {
    buf_.append(v);
  }
  return *this;
}

CsvWriter& CsvWriter::Field(std::int64_t v) {
  WriteSeparatorIfNeeded();
  char tmp[24];
  const auto res = std::to_chars(tmp, tmp + sizeof(tmp), v);
  buf_.append(tmp, static_cast<std::size_t>(res.ptr - tmp));
  return *this;
}

CsvWriter& CsvWriter::Field(std::uint64_t v) {
  WriteSeparatorIfNeeded();
  char tmp[24];
  const auto res = std::to_chars(tmp, tmp + sizeof(tmp), v);
  buf_.append(tmp, static_cast<std::size_t>(res.ptr - tmp));
  return *this;
}

std::string CsvWriter::FormatDouble(double v, int precision) {
  if (std::isnan(v)) {
    return "nan";
  }
  if (std::isinf(v)) {
    return v > 0 ? "inf" : "-inf";
  }
  if (precision < 0) {
    precision = 0;
  }
  if (precision > kMaxPrecision) {
    precision = kMaxPrecision;
  }
  // std::to_chars with fixed notation is locale-independent and correctly
  // rounded -- the two properties operator<< does not give us.
  char tmp[64];
  const auto res = std::to_chars(tmp, tmp + sizeof(tmp), v, std::chars_format::fixed, precision);
  if (res.ec != std::errc{}) {
    return "nan";
  }
  std::string s(tmp, static_cast<std::size_t>(res.ptr - tmp));
  // Normalise "-0.000000" to "0.000000": the sign of a zero is an artefact of
  // the arithmetic, not information, and it would break byte-comparison across
  // platforms that round differently at the last place.
  if (s.find_first_not_of("-0.") == std::string::npos && s.front() == '-') {
    s.erase(s.begin());
  }
  return s;
}

CsvWriter& CsvWriter::Field(double v, int precision) {
  WriteSeparatorIfNeeded();
  buf_.append(FormatDouble(v, precision));
  return *this;
}

void CsvWriter::EndRow() {
  if (columns_ != 0 && field_index_ != columns_) {
    throw std::logic_error("CsvWriter(" + path_ + "): row has " + std::to_string(field_index_) +
                           " fields but the header declares " + std::to_string(columns_));
  }
  buf_.push_back('\n');
  field_index_ = 0;
  ++rows_;
  if (buf_.size() >= (1U << 16)) {
    Flush();
  }
}

void CsvWriter::Flush() {
  if (!buf_.empty() && out_.is_open()) {
    out_.write(buf_.data(), static_cast<std::streamsize>(buf_.size()));
    buf_.clear();
  }
  if (out_.is_open()) {
    out_.flush();
  }
}

void CsvWriter::Close() {
  if (!out_.is_open()) {
    return;
  }
  Flush();
  out_.close();
}

}  // namespace lob
