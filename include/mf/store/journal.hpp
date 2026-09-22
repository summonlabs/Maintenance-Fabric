#pragma once

// Append-only write-ahead journal with per-frame integrity and transaction
// atomicity. Recovery is conservative: the first frame that fails a header
// checksum, a payload checksum, a length bound, a type check or a sequence
// check ends the replay, and the file is truncated exactly there. A partially
// written tail can therefore never be mistaken for committed state.

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "mf/core/id.hpp"
#include "mf/core/limits.hpp"
#include "mf/core/result.hpp"
#include "mf/store/records.hpp"

namespace mf {

struct JournalOptions {
  std::string path;
  bool create_if_missing{true};
  bool create_parent_directories{true};
  std::uint64_t max_bytes{limits::kMaxJournalBytes};
  std::uint64_t max_records{limits::kMaxJournalRecords};
};

struct ReplayStats {
  std::uint64_t frames{0};
  std::uint64_t records{0};
  std::uint64_t bytes{0};
  std::uint64_t discarded_frames{0};
  std::uint64_t discarded_records{0};
  std::uint64_t truncated_bytes{0};
  std::uint64_t first_bad_offset{0};
  bool truncated{false};
  std::string detail;
};

class Journal {
 public:
  Journal() = default;
  ~Journal();
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;
  Journal(Journal&& other) noexcept;
  Journal& operator=(Journal&& other) noexcept;

  [[nodiscard]] static Result<Journal> open(const JournalOptions& options);

  /// Streams every intact frame to \p visitor in append order. Frames after the
  /// first damaged frame are not delivered and the file is truncated there.
  [[nodiscard]] Status replay(const std::function<void(const Record&)>& visitor,
                              ReplayStats& stats);

  [[nodiscard]] Status append(const Record& record);
  [[nodiscard]] Status append_transaction(const std::vector<Record>& records);
  [[nodiscard]] Status flush();

  /// Atomically replaces the journal contents with \p records (compaction).
  [[nodiscard]] Status rewrite(const std::vector<Record>& records);

  [[nodiscard]] RecordSeq next_sequence() const noexcept { return next_seq_; }
  [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::uint64_t records() const noexcept { return records_; }
  [[nodiscard]] const std::string& path() const noexcept { return options_.path; }
  [[nodiscard]] bool open() const noexcept { return file_ != nullptr; }

  [[nodiscard]] Status sync();
  void close();

  friend struct JournalTestAccess;

 private:
  [[nodiscard]] Status write_frame(RecordType type, std::uint64_t sequence,
                                   const std::vector<std::byte>& payload);
  [[nodiscard]] Status ensure_replayed() const;

  bool replayed_{false};
  std::FILE* file_{nullptr};
  JournalOptions options_;
  std::uint64_t bytes_{0};
  std::uint64_t records_{0};
  RecordSeq next_seq_{RecordSeq::from_u64(1)};
};

/// Rejects a journal file whose header magic or layout revision is unknown.
[[nodiscard]] Status validate_journal_header(std::uint32_t magic, std::uint16_t version);

}  // namespace mf
