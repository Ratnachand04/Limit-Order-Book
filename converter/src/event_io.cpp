#include <lob/converter/event_io.hpp>

#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <system_error>

namespace lob {
namespace {

// 64k records = 2 MiB per block: large enough that the syscall cost disappears
// against the per-event work, small enough to stay resident in L2/L3.
constexpr std::size_t kBlockRecords = 64 * 1024;

}  // namespace

// ---------------------------------------------------------------------------
// EventWriter
// ---------------------------------------------------------------------------
EventWriter::EventWriter(const std::string& path) { Open(path); }

EventWriter::~EventWriter() {
  if (out_.is_open()) {
    try {
      Close();
    } catch (...) {
      // A destructor may not throw.  The file is left with the placeholder
      // header, which Open()/EventReader treats as "count unknown, scan".
    }
  }
}

void EventWriter::Open(const std::string& path) {
  path_ = path;
  const std::filesystem::path fs_path(path);
  if (fs_path.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(fs_path.parent_path(), ec);
  }
  out_.open(path, std::ios::binary | std::ios::trunc);
  if (!out_) {
    throw std::runtime_error("EventWriter: cannot open " + path + " for writing");
  }
  header_ = EventFileHeader{};
  have_first_ = false;
  buffer_.clear();
  buffer_.reserve(kBlockRecords);
  // Placeholder header; rewritten by Close() once the counts are known.
  out_.write(reinterpret_cast<const char*>(&header_), sizeof(header_));
}

void EventWriter::Write(const Event& e) {
  if (!have_first_) {
    header_.first_ts_us = e.exch_ts_us;
    have_first_ = true;
  }
  header_.last_ts_us = e.exch_ts_us;
  ++header_.record_count;
  buffer_.push_back(e);
  if (buffer_.size() >= kBlockRecords) {
    FlushBuffer();
  }
}

void EventWriter::Write(const Event* events, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    Write(events[i]);
  }
}

void EventWriter::FlushBuffer() {
  if (buffer_.empty()) {
    return;
  }
  out_.write(reinterpret_cast<const char*>(buffer_.data()),
             static_cast<std::streamsize>(buffer_.size() * sizeof(Event)));
  if (!out_) {
    throw std::runtime_error("EventWriter: write failed on " + path_);
  }
  buffer_.clear();
}

void EventWriter::Close() {
  if (!out_.is_open()) {
    return;
  }
  FlushBuffer();
  out_.seekp(0, std::ios::beg);
  out_.write(reinterpret_cast<const char*>(&header_), sizeof(header_));
  out_.flush();
  out_.close();
}

// ---------------------------------------------------------------------------
// EventReader
// ---------------------------------------------------------------------------
EventReader::EventReader(const std::string& path) { Open(path); }

void EventReader::Open(const std::string& path) {
  path_ = path;
  in_.open(path, std::ios::binary);
  if (!in_) {
    throw std::runtime_error("EventReader: cannot open " + path);
  }
  in_.read(reinterpret_cast<char*>(&header_), sizeof(header_));
  if (in_.gcount() != static_cast<std::streamsize>(sizeof(header_))) {
    throw std::runtime_error("EventReader: " + path + " is shorter than one header");
  }
  if (std::memcmp(header_.magic, "LOB1", 4) != 0) {
    throw std::runtime_error("EventReader: " + path + " is not a LOB1 event file");
  }
  if (header_.version != kEventFileVersion) {
    throw std::runtime_error("EventReader: " + path + " has schema version " +
                             std::to_string(header_.version) + ", expected " +
                             std::to_string(kEventFileVersion));
  }
  if (header_.record_size != kEventSize) {
    throw std::runtime_error("EventReader: " + path + " has record size " +
                             std::to_string(header_.record_size) + ", expected " +
                             std::to_string(kEventSize));
  }
  block_.resize(kBlockRecords);
  block_size_ = 0;
  block_pos_ = 0;
  consumed_ = 0;
}

bool EventReader::Refill() {
  if (!in_) {
    return false;
  }
  in_.read(reinterpret_cast<char*>(block_.data()),
           static_cast<std::streamsize>(block_.size() * sizeof(Event)));
  const std::streamsize bytes = in_.gcount();
  if (bytes <= 0) {
    block_size_ = 0;
    block_pos_ = 0;
    return false;
  }
  if (bytes % static_cast<std::streamsize>(sizeof(Event)) != 0) {
    throw std::runtime_error("EventReader: " + path_ + " ends mid-record (truncated file)");
  }
  block_size_ = static_cast<std::size_t>(bytes) / sizeof(Event);
  block_pos_ = 0;
  return true;
}

bool EventReader::Next(Event& out) {
  if (block_pos_ >= block_size_ && !Refill()) {
    return false;
  }
  out = block_[block_pos_++];
  ++consumed_;
  return true;
}

std::size_t EventReader::NextBlock(const Event*& out) {
  if (block_pos_ >= block_size_ && !Refill()) {
    out = nullptr;
    return 0;
  }
  out = block_.data() + block_pos_;
  const std::size_t n = block_size_ - block_pos_;
  block_pos_ = block_size_;
  consumed_ += n;
  return n;
}

// ---------------------------------------------------------------------------
// Whole-file helpers
// ---------------------------------------------------------------------------
std::vector<Event> ReadAllEvents(const std::string& path) {
  EventReader reader(path);
  std::vector<Event> all;
  if (reader.header().record_count > 0) {
    all.reserve(static_cast<std::size_t>(reader.header().record_count));
  }
  const Event* block = nullptr;
  std::size_t n = 0;
  while ((n = reader.NextBlock(block)) > 0) {
    all.insert(all.end(), block, block + n);
  }
  return all;
}

void WriteAllEvents(const std::string& path, const std::vector<Event>& events) {
  EventWriter writer(path);
  writer.Write(events.data(), events.size());
  writer.Close();
}

}  // namespace lob
