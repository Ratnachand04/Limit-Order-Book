// Binary Event file I/O.
//
// Format: a 32-byte header followed by a dense array of 32-byte Event records
// (master plan §4.3).  No compression and no per-record framing -- the whole
// point of the binary stage is that replay can mmap or block-read straight into
// an Event array with zero parsing (Part 11, pitfall #2).
#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <lob/types.hpp>

namespace lob {

// File header.  Deliberately the same size as an Event so the record array
// stays 32-byte aligned and `record_index = (offset - 32) / 32`.
struct EventFileHeader {
  char magic[4] = {'L', 'O', 'B', '1'};                                  // offset  0
  std::uint16_t version = 1;                                             // offset  4
  std::uint16_t record_size = static_cast<std::uint16_t>(kEventSize);    // offset  6
  std::uint64_t record_count = 0;  // offset  8; patched on Close()
  Ts first_ts_us = 0;              // offset 16
  Ts last_ts_us = 0;               // offset 24
};
static_assert(sizeof(EventFileHeader) == 32, "header must match the record stride");
static_assert(alignof(EventFileHeader) == 8);
static_assert(offsetof(EventFileHeader, record_count) == 8);
static_assert(offsetof(EventFileHeader, first_ts_us) == 16);
static_assert(offsetof(EventFileHeader, last_ts_us) == 24);

inline constexpr std::uint16_t kEventFileVersion = 1;

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------
class EventWriter {
 public:
  EventWriter() = default;
  explicit EventWriter(const std::string& path);
  ~EventWriter();

  EventWriter(const EventWriter&) = delete;
  EventWriter& operator=(const EventWriter&) = delete;

  void Open(const std::string& path);
  void Write(const Event& e);
  void Write(const Event* events, std::size_t n);
  // Rewrites the header with the final counts and timestamps, then closes.
  void Close();

  [[nodiscard]] std::uint64_t count() const { return header_.record_count; }
  [[nodiscard]] bool is_open() const { return out_.is_open(); }

 private:
  void FlushBuffer();

  std::ofstream out_;
  std::string path_;
  EventFileHeader header_;
  std::vector<Event> buffer_;
  bool have_first_ = false;
};

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------
// Block-buffered sequential reader.  Reading in 64k-record blocks keeps the
// replay loop free of per-event syscalls.
class EventReader {
 public:
  EventReader() = default;
  explicit EventReader(const std::string& path);

  void Open(const std::string& path);
  [[nodiscard]] const EventFileHeader& header() const { return header_; }

  // Returns false at end of file.
  bool Next(Event& out);

  // Bulk access to the current block; advances past it.  Returns 0 at EOF.
  std::size_t NextBlock(const Event*& out);

  [[nodiscard]] std::uint64_t position() const { return consumed_; }

 private:
  bool Refill();

  std::ifstream in_;
  std::string path_;
  EventFileHeader header_;
  std::vector<Event> block_;
  std::size_t block_size_ = 0;
  std::size_t block_pos_ = 0;
  std::uint64_t consumed_ = 0;
};

// Reads a whole file into memory.  Convenient for tests and for replays whose
// input comfortably fits in RAM (a day of one symbol is a few hundred MB).
std::vector<Event> ReadAllEvents(const std::string& path);

// Writes a whole vector in one call.  Used by tests and fixture generators.
void WriteAllEvents(const std::string& path, const std::vector<Event>& events);

}  // namespace lob
