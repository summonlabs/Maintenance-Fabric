// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "mf/store/journal.hpp"

#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

#include "mf/core/bytes.hpp"
#include "mf/core/checked.hpp"
#include "mf/core/hash.hpp"
#include "mf/version.hpp"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace mf {
namespace {

constexpr std::uint32_t kMagic = 0x4D464A31u;  // "MFJ1"
constexpr std::uint16_t kJournalVersion = 1;
constexpr std::size_t kFileHeaderBytes = 12;
constexpr std::size_t kFrameHeaderBytes = 24;

std::uint32_t read_u32_le(const unsigned char* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24u) | (static_cast<std::uint32_t>(p[1]) << 16u) |
         (static_cast<std::uint32_t>(p[2]) << 8u) | static_cast<std::uint32_t>(p[3]);
}

std::uint64_t read_u64_be(const unsigned char* p) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8u) | static_cast<std::uint64_t>(p[i]);
  }
  return value;
}

void write_u32_be(unsigned char* p, std::uint32_t value) {
  p[0] = static_cast<unsigned char>((value >> 24u) & 0xFFu);
  p[1] = static_cast<unsigned char>((value >> 16u) & 0xFFu);
  p[2] = static_cast<unsigned char>((value >> 8u) & 0xFFu);
  p[3] = static_cast<unsigned char>(value & 0xFFu);
}

void write_u64_be(unsigned char* p, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<unsigned char>((value >> (8 * (7 - i))) & 0xFFull);
  }
}

Status sync_file(std::FILE* file) {
  if (std::fflush(file) != 0) {
    return make_error(ErrorCode::IoError, "failed to flush the journal");
  }
#ifdef _WIN32
  if (_commit(_fileno(file)) != 0) {
    return make_error(ErrorCode::IoError, "failed to commit the journal to disk");
  }
#else
  if (::fsync(::fileno(file)) != 0) {
    return make_error(ErrorCode::IoError, "failed to fsync the journal");
  }
#endif
  return ok_status();
}

bool seek_end(std::FILE* file);

Status seek_file(std::FILE* file, std::uint64_t offset) {
#ifdef _WIN32
  if (_fseeki64(file, static_cast<__int64>(offset), SEEK_SET) != 0) {
    return make_error(ErrorCode::IoError, "cannot seek the journal");
  }
#else
  if (::fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0) {
    return make_error(ErrorCode::IoError, "cannot seek the journal");
  }
#endif
  return ok_status();
}

Status truncate_file(std::FILE* file, std::uint64_t size) {
  if (file == nullptr) {
    return make_error(ErrorCode::IoError, "no journal file handle");
  }
#ifdef _WIN32
  if (_chsize_s(_fileno(file), static_cast<long long>(size)) != 0) {
    return make_error(ErrorCode::IoError, "failed to truncate the journal");
  }
#else
  if (::ftruncate(::fileno(file), static_cast<off_t>(size)) != 0) {
    return make_error(ErrorCode::IoError, "failed to truncate the journal");
  }
#endif
  return ok_status();
}

bool seek_end(std::FILE* file) {
#ifdef _WIN32
  return _fseeki64(file, 0, SEEK_END) == 0;
#else
  return std::fseek(file, 0, SEEK_END) == 0;
#endif
}

std::uint64_t file_size_of(std::FILE* file) {
#ifdef _WIN32
  const __int64 current = _ftelli64(file);
  if (current < 0) {
    return 0;
  }
  if (_fseeki64(file, 0, SEEK_END) != 0) {
    return 0;
  }
  const __int64 end = _ftelli64(file);
  if (_fseeki64(file, current, SEEK_SET) != 0) {
    return 0;
  }
  return end < 0 ? 0 : static_cast<std::uint64_t>(end);
#else
  const long current = std::ftell(file);
  if (current < 0) {
    return 0;
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    return 0;
  }
  const long end = std::ftell(file);
  if (std::fseek(file, current, SEEK_SET) != 0) {
    return 0;
  }
  return end < 0 ? 0 : static_cast<std::uint64_t>(end);
#endif
}

}  // namespace

Status validate_journal_header(std::uint32_t magic, std::uint16_t version) {
  if (magic != kMagic) {
    return make_error(ErrorCode::CorruptState, "journal magic does not match Maintenance Fabric");
  }
  if (version != kJournalVersion) {
    return make_error(ErrorCode::VersionMismatch,
                      "journal layout revision " + std::to_string(version) +
                          " is not supported by this runtime (expected " +
                          std::to_string(kJournalVersion) + ")");
  }
  return ok_status();
}

Journal::~Journal() { close(); }

Journal::Journal(Journal&& other) noexcept
    : file_(other.file_),
      options_(std::move(other.options_)),
      bytes_(other.bytes_),
      records_(other.records_),
      next_seq_(other.next_seq_) {
  other.file_ = nullptr;
  other.bytes_ = 0;
  other.records_ = 0;
}

Journal& Journal::operator=(Journal&& other) noexcept {
  if (this != &other) {
    close();
    file_ = other.file_;
    options_ = std::move(other.options_);
    bytes_ = other.bytes_;
    records_ = other.records_;
    next_seq_ = other.next_seq_;
    other.file_ = nullptr;
    other.bytes_ = 0;
    other.records_ = 0;
  }
  return *this;
}

Result<Journal> Journal::open(const JournalOptions& options) {
  if (options.path.empty()) {
    return invalid_argument("journal path is empty");
  }
  Journal journal;
  journal.options_ = options;

  if (options.create_parent_directories) {
    const std::filesystem::path path(options.path);
    if (path.has_parent_path()) {
      std::error_code error;
      std::filesystem::create_directories(path.parent_path(), error);
      if (error) {
        return make_error(ErrorCode::IoError,
                          "cannot create journal directory: " + error.message());
      }
    }
  }

  const bool exists = std::filesystem::exists(options.path);
  if (!exists && !options.create_if_missing) {
    return make_error(ErrorCode::NotFound, "journal file does not exist: " + options.path);
  }

  std::FILE* file = std::fopen(options.path.c_str(), exists ? "rb+" : "wb+");
  if (file == nullptr) {
    return make_error(ErrorCode::IoError, "cannot open journal file: " + options.path);
  }
  journal.file_ = file;

  const std::uint64_t size = file_size_of(file);
  if (size == 0 && !exists) {
    unsigned char header[kFileHeaderBytes];
    write_u32_be(header + 0, kMagic);
    header[4] = static_cast<unsigned char>((kJournalVersion >> 8u) & 0xFFu);
    header[5] = static_cast<unsigned char>(kJournalVersion & 0xFFu);
    write_u32_be(header + 6, 0);
    header[10] = 0;
    header[11] = 0;
    if (std::fwrite(header, 1, sizeof(header), file) != sizeof(header)) {
      journal.close();
      return make_error(ErrorCode::IoError, "cannot write the journal header");
    }
    const Status synced = sync_file(file);
    if (!synced.ok()) {
      journal.close();
      return synced.error();
    }
    journal.bytes_ = kFileHeaderBytes;
    journal.replayed_ = true;
    return Result<Journal>(std::move(journal));
  }
  if (size < kFileHeaderBytes) {
    journal.close();
    return make_error(ErrorCode::CorruptState, "journal is shorter than its header");
  }
  const Status sought = seek_file(file, 0);
  if (!sought.ok()) {
    journal.close();
    return sought.error();
  }
  unsigned char header[kFileHeaderBytes];
  if (std::fread(header, 1, sizeof(header), file) != sizeof(header)) {
    journal.close();
    return make_error(ErrorCode::IoError, "cannot read the journal header");
  }
  const Status valid = validate_journal_header(read_u32_le(header), 
                                               static_cast<std::uint16_t>((static_cast<std::uint16_t>(header[4]) << 8u) |
                                                                          static_cast<std::uint16_t>(header[5])));
  if (!valid.ok()) {
    journal.close();
    return valid.error();
  }
  journal.bytes_ = size;
  return Result<Journal>(std::move(journal));
}

Status Journal::replay(const std::function<void(const Record&)>& visitor, ReplayStats& stats) {
  stats = ReplayStats{};
  if (file_ == nullptr) {
    return make_error(ErrorCode::IoError, "journal is not open");
  }
  const std::uint64_t size = file_size_of(file_);
  if (size < kFileHeaderBytes) {
    return make_error(ErrorCode::CorruptState, "journal is shorter than its header");
  }
  const Status sought = seek_file(file_, kFileHeaderBytes);
  if (!sought.ok()) {
    return sought;
  }

  std::uint64_t offset = kFileHeaderBytes;
  std::uint64_t expected_sequence = 1;
  std::vector<std::byte> buffer;
  std::vector<unsigned char> header(kFrameHeaderBytes);
  bool stopped = false;

  while (!stopped && offset < size) {
    const std::size_t got = std::fread(header.data(), 1, kFrameHeaderBytes, file_);
    if (got == 0) {
      break;
    }
    if (got < kFrameHeaderBytes) {
      stats.detail = "final frame header is truncated";
      stopped = true;
      break;
    }
    const std::uint32_t header_crc = read_u32_le(header.data() + 20);
    if (crc32c(header.data(), 20) != header_crc) {
      stats.detail = "frame header checksum mismatch";
      stopped = true;
      break;
    }
    const std::uint16_t type = static_cast<std::uint16_t>((static_cast<std::uint16_t>(header[0]) << 8u) |
                                                          static_cast<std::uint16_t>(header[1]));
    const std::uint64_t sequence = read_u64_be(header.data() + 4);
    const std::uint32_t length = read_u32_le(header.data() + 12);
    const std::uint32_t payload_crc = read_u32_le(header.data() + 16);

    if (!is_known_record_type(type)) {
      stats.detail = "frame declares an unrecognised record type";
      stopped = true;
      break;
    }
    if (length > limits::kMaxJournalRecordBytes) {
      stats.detail = "frame length exceeds the accepted bound";
      stopped = true;
      break;
    }
    if (sequence != expected_sequence) {
      stats.detail = "frame sequence is out of order";
      stopped = true;
      break;
    }
    buffer.resize(length);
    if (length > 0) {
      const std::size_t payload_got = std::fread(buffer.data(), 1, length, file_);
      if (payload_got < length) {
        stats.detail = "final frame payload is truncated";
        stopped = true;
        break;
      }
    }
    if (crc32c(buffer.data(), buffer.size()) != payload_crc) {
      stats.detail = "frame payload checksum mismatch";
      stopped = true;
      break;
    }

    Record record;
    record.type = static_cast<RecordType>(type);
    record.payload = buffer;
    if (record.type == RecordType::Transaction) {
      std::vector<Record> inner;
      const Status decoded = decode_transaction_record(record, inner);
      if (!decoded.ok()) {
        stats.detail = "transaction frame could not be decoded";
        stopped = true;
        break;
      }
      for (const Record& sub : inner) {
        visitor(sub);
        ++stats.records;
      }
    } else {
      visitor(record);
      ++stats.records;
    }
    ++stats.frames;
    offset += kFrameHeaderBytes + length;
    ++expected_sequence;
  }

  stats.bytes = offset;
  stats.truncated = offset < size;
  stats.first_bad_offset = offset;
  stats.truncated_bytes = size - offset;
  if (stats.truncated) {
    if (stats.detail.empty()) {
      stats.detail = "journal ends with bytes that are not a complete frame";
    }
    const Status truncated = truncate_file(file_, offset);
    if (!truncated.ok()) {
      return truncated;
    }
    const Status synced = sync_file(file_);
    if (!synced.ok()) {
      return synced;
    }
  }
  next_seq_ = RecordSeq::from_u64(expected_sequence);
  records_ = stats.frames;
  bytes_ = offset;
  if (!seek_end(file_)) {
    return make_error(ErrorCode::IoError, "cannot seek the journal to its end");
  }
  replayed_ = true;
  return ok_status();
}

Status Journal::write_frame(RecordType type, std::uint64_t sequence,
                            const std::vector<std::byte>& payload) {
  if (file_ == nullptr) {
    return make_error(ErrorCode::IoError, "journal is not open");
  }
  if (payload.size() > limits::kMaxJournalRecordBytes) {
    return make_error(ErrorCode::LimitExceeded, "record payload exceeds the journal bound");
  }
  const std::optional<std::uint64_t> frame_bytes =
      checked_add<std::uint64_t>(kFrameHeaderBytes, static_cast<std::uint64_t>(payload.size()));
  if (!frame_bytes.has_value()) {
    return make_error(ErrorCode::Overflow, "journal frame size overflowed");
  }
  const std::optional<std::uint64_t> projected = checked_add<std::uint64_t>(bytes_, *frame_bytes);
  if (!projected.has_value() || *projected > options_.max_bytes) {
    return make_error(ErrorCode::LimitExceeded,
                      "journal would exceed its configured byte bound; compaction is required");
  }
  if (records_ >= options_.max_records) {
    return make_error(ErrorCode::LimitExceeded,
                      "journal would exceed its configured record bound; compaction is required");
  }

  unsigned char header[kFrameHeaderBytes];
  header[0] = static_cast<unsigned char>((static_cast<std::uint16_t>(type) >> 8u) & 0xFFu);
  header[1] = static_cast<unsigned char>(static_cast<std::uint16_t>(type) & 0xFFu);
  header[2] = 0;
  header[3] = 0;
  write_u64_be(header + 4, sequence);
  write_u32_be(header + 12, static_cast<std::uint32_t>(payload.size()));
  write_u32_be(header + 16, crc32c(payload.data(), payload.size()));
  write_u32_be(header + 20, crc32c(header, 20));

  if (std::fwrite(header, 1, sizeof(header), file_) != sizeof(header)) {
    return make_error(ErrorCode::IoError, "failed to append a journal frame header");
  }
  if (!payload.empty() &&
      std::fwrite(payload.data(), 1, payload.size(), file_) != payload.size()) {
    return make_error(ErrorCode::IoError, "failed to append a journal frame payload");
  }
  bytes_ = *projected;
  ++records_;
  return ok_status();
}

Status Journal::append(const Record& record) {
  const Status valid = validate_record(record);
  if (!valid.ok()) {
    return valid;
  }
  if (record.type == RecordType::Transaction) {
    return invalid_argument("use append_transaction for transactional records");
  }
  const Status admitted = ensure_replayed();
  if (!admitted.ok()) {
    return admitted;
  }
  const Status written = write_frame(record.type, next_seq_.value(), record.payload);
  if (!written.ok()) {
    return written;
  }
  next_seq_ = next_seq_.next();
  return ok_status();
}

Status Journal::append_transaction(const std::vector<Record>& records) {
  if (records.empty()) {
    return invalid_argument("transaction must contain at least one record");
  }
  if (records.size() > limits::kMaxRecordsPerTransaction) {
    return make_error(ErrorCode::LimitExceeded, "transaction contains too many records");
  }
  for (const Record& record : records) {
    const Status valid = validate_record(record);
    if (!valid.ok()) {
      return valid;
    }
    if (record.type == RecordType::Transaction) {
      return invalid_argument("transactions cannot nest");
    }
  }
  const Status admitted = ensure_replayed();
  if (!admitted.ok()) {
    return admitted;
  }
  const Record transaction = encode_transaction_record(records);
  if (transaction.payload.size() > limits::kMaxJournalRecordBytes) {
    return make_error(ErrorCode::LimitExceeded, "transaction exceeds the journal record bound");
  }
  const Status written = write_frame(transaction.type, next_seq_.value(), transaction.payload);
  if (!written.ok()) {
    return written;
  }
  next_seq_ = next_seq_.next();
  return ok_status();
}

Status Journal::ensure_replayed() const {
  if (!replayed_) {
    return make_error(ErrorCode::StateConflict,
                      "journal must be replayed before it accepts appends");
  }
  return ok_status();
}

Status Journal::flush() {
  if (file_ == nullptr) {
    return make_error(ErrorCode::IoError, "journal is not open");
  }
  return sync_file(file_);
}

Status Journal::sync() { return flush(); }

Status Journal::rewrite(const std::vector<Record>& records) {
  if (records.size() > options_.max_records) {
    return make_error(ErrorCode::LimitExceeded, "compacted journal would exceed the record bound");
  }
  const std::string temporary = options_.path + ".compact";
  std::FILE* out = std::fopen(temporary.c_str(), "wb+");
  if (out == nullptr) {
    return make_error(ErrorCode::IoError, "cannot create the compaction target");
  }
  unsigned char header[kFileHeaderBytes];
  write_u32_be(header + 0, kMagic);
  header[4] = static_cast<unsigned char>((kJournalVersion >> 8u) & 0xFFu);
  header[5] = static_cast<unsigned char>(kJournalVersion & 0xFFu);
  write_u32_be(header + 6, 0);
  header[10] = 0;
  header[11] = 0;
  std::uint64_t bytes = kFileHeaderBytes;
  if (std::fwrite(header, 1, sizeof(header), out) != sizeof(header)) {
    std::fclose(out);
    std::filesystem::remove(temporary);
    return make_error(ErrorCode::IoError, "cannot write the compaction header");
  }
  std::uint64_t sequence = 1;
  for (const Record& record : records) {
    const Status valid = validate_record(record);
    if (!valid.ok()) {
      std::fclose(out);
      std::filesystem::remove(temporary);
      return valid;
    }
    const Record encoded = record.type == RecordType::Transaction
                               ? record
                               : encode_transaction_record({record});
    if (encoded.payload.size() > limits::kMaxJournalRecordBytes) {
      std::fclose(out);
      std::filesystem::remove(temporary);
      return make_error(ErrorCode::LimitExceeded, "compaction record exceeds the journal bound");
    }
    unsigned char frame[kFrameHeaderBytes];
    frame[0] = static_cast<unsigned char>((static_cast<std::uint16_t>(encoded.type) >> 8u) & 0xFFu);
    frame[1] = static_cast<unsigned char>(static_cast<std::uint16_t>(encoded.type) & 0xFFu);
    frame[2] = 0;
    frame[3] = 0;
    write_u64_be(frame + 4, sequence);
    write_u32_be(frame + 12, static_cast<std::uint32_t>(encoded.payload.size()));
    write_u32_be(frame + 16, crc32c(encoded.payload.data(), encoded.payload.size()));
    write_u32_be(frame + 20, crc32c(frame, 20));
    if (std::fwrite(frame, 1, sizeof(frame), out) != sizeof(frame)) {
      std::fclose(out);
      std::filesystem::remove(temporary);
      return make_error(ErrorCode::IoError, "cannot write a compaction frame header");
    }
    if (!encoded.payload.empty() &&
        std::fwrite(encoded.payload.data(), 1, encoded.payload.size(), out) !=
            encoded.payload.size()) {
      std::fclose(out);
      std::filesystem::remove(temporary);
      return make_error(ErrorCode::IoError, "cannot write a compaction frame payload");
    }
    bytes += kFrameHeaderBytes + encoded.payload.size();
    ++sequence;
    if (bytes > options_.max_bytes) {
      std::fclose(out);
      std::filesystem::remove(temporary);
      return make_error(ErrorCode::LimitExceeded, "compacted journal exceeds its byte bound");
    }
  }
  const Status synced = sync_file(out);
  if (!synced.ok()) {
    std::fclose(out);
    std::filesystem::remove(temporary);
    return synced;
  }
  if (std::fclose(out) != 0) {
    std::filesystem::remove(temporary);
    return make_error(ErrorCode::IoError, "cannot close the compaction target");
  }
  close();
  std::error_code error;
  std::filesystem::rename(temporary, options_.path, error);
  if (error) {
    std::filesystem::remove(temporary);
    return make_error(ErrorCode::IoError, "cannot install the compacted journal: " + error.message());
  }
  Result<Journal> reopened = Journal::open(options_);
  if (!reopened.ok()) {
    return reopened.error();
  }
  const RecordSeq sequence_before = next_seq_;
  *this = std::move(reopened.value());
  next_seq_ = sequence_before;
  records_ = records.size();
  if (!seek_end(file_)) {
    return make_error(ErrorCode::IoError, "cannot seek the compacted journal to its end");
  }
  replayed_ = true;
  return ok_status();
}

void Journal::close() {
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
}

}  // namespace mf
