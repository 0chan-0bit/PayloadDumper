#include "payload.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include "bspatch.h"
#include "decompress.h"
#include "operation_scheduler.h"
#include "sha.h"
#include "verity.h"
#include "zip.h"

#include "proto/update_metadata.pb.h"

namespace pd {
namespace {

constexpr size_t kHeaderLen = 24;
constexpr size_t kCopyBufferSize = 4u << 20;

uint64_t rd64be(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}
uint32_t rd32be(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

std::vector<uint8_t> to_bytes(const std::string& s) {
  return std::vector<uint8_t>(s.begin(), s.end());
}

void add_extent(Extents& out, const chromeos_update_engine::Extent& e) {
  if (e.num_blocks() != 0) {
    out.push_back({e.start_block(), e.num_blocks()});
  }
}

bool operation_needs_source(const Operation& op) {
  if (!op.src_extents.empty()) return true;
  switch (op.type) {
    case OpType::kMove:
    case OpType::kBsdiff:
    case OpType::kSourceCopy:
    case OpType::kSourceBsdiff:
    case OpType::kBrotliBsdiff:
    case OpType::kPuffdiff:
    case OpType::kZucchini:
    case OpType::kLz4diffBsdiff:
    case OpType::kLz4diffPuffdiff:
      return true;
    default:
      return false;
  }
}

bool operation_supported(OpType type) {
  switch (type) {
    case OpType::kReplace:
    case OpType::kReplaceBz:
    case OpType::kReplaceXz:
    case OpType::kZstd:
    case OpType::kZero:
    case OpType::kDiscard:
    case OpType::kMove:
    case OpType::kBsdiff:
    case OpType::kSourceCopy:
    case OpType::kSourceBsdiff:
    case OpType::kBrotliBsdiff:
      return true;
    default:
      return false;
  }
}

bool needs_full_source_verification(const Partition& part) {
  for (const auto& op : part.operations) {
    if (operation_needs_source(op) && op.src_sha256.empty()) return true;
  }
  return false;
}

std::string join(const std::vector<std::string>& items, const char* sep) {
  std::string out;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) out += sep;
    out += items[i];
  }
  return out;
}

// ---------------------------------------------------------------- job state

struct Job {
  const Partition* part = nullptr;
  OutFile out;
  MappedFile source;
  bool has_source = false;
  std::atomic<int> remaining{0};
  std::atomic<int> completed{0};
  std::atomic<bool> failed{false};
  std::mutex error_mu;
  std::string error;
  bool fec_skipped = false;
  bool output_verified = false;
};

void set_error(Job& job, const std::string& message) {
  std::lock_guard<std::mutex> lock(job.error_mu);
  if (!job.failed.load(std::memory_order_relaxed)) {
    job.error = message;
    job.failed.store(true, std::memory_order_relaxed);
  }
}

std::string get_error(Job& job) {
  std::lock_guard<std::mutex> lock(job.error_mu);
  return job.error;
}

template <typename F>
void guarded(Job& job, F&& fn) {
  try {
    fn();
  } catch (const std::exception& e) {
    set_error(job, e.what());
  } catch (...) {
    set_error(job, "unknown error");
  }
}

// ---------------------------------------------------------------- sinks

class DirectSink : public Sink {
 public:
  DirectSink(uint8_t* dst, uint64_t capacity) : dst_(dst), capacity_(capacity) {}
  uint8_t* prepare(size_t* capacity) override {
    *capacity = static_cast<size_t>(
        std::min<uint64_t>(capacity_ - produced_, SIZE_MAX / 2));
    if (*capacity == 0) throw Error("decompression: output exceeds destination");
    return dst_ + produced_;
  }
  void commit(size_t n) override {
    if (stop_requested()) throw Error("payload: interrupted");
    produced_ += n;
    if (produced_ > capacity_) {
      throw Error("decompression: output exceeds destination extents");
    }
  }
  uint64_t produced() const override { return produced_; }

 private:
  uint8_t* dst_;
  uint64_t capacity_;
  uint64_t produced_ = 0;
};

class ScatterSink : public Sink {
 public:
  ScatterSink(ExtentWriter& writer, std::vector<uint8_t>& buffer)
      : writer_(writer), buffer_(buffer) {}
  uint8_t* prepare(size_t* capacity) override {
    *capacity = buffer_.size();
    return buffer_.data();
  }
  void commit(size_t n) override {
    if (stop_requested()) throw Error("payload: interrupted");
    writer_.write(buffer_.data(), n);
    produced_ += n;
  }
  uint64_t produced() const override { return produced_; }

 private:
  ExtentWriter& writer_;
  std::vector<uint8_t>& buffer_;
  uint64_t produced_ = 0;
};

std::vector<uint8_t>& thread_scratch() {
  thread_local std::vector<uint8_t> buffer(kCopyBufferSize);
  return buffer;
}

// ------------------------------------------------------------ verification

void check_digest(const std::string& partition, const char* subject,
                  const std::vector<uint8_t>& expected,
                  const uint8_t* actual, size_t actual_len) {
  if (expected.empty()) return;
  if (expected.size() != actual_len ||
      !digest_equal(expected.data(), actual, actual_len)) {
    throwf("payload: %s verification failed for partition \"%s\": expected "
           "sha256 %s, got %s",
           subject, partition.c_str(), hex(expected).c_str(),
           hex(actual, actual_len).c_str());
  }
}

void verify_blob(const Partition& part, const Operation& op, Span blob) {
  if (op.data_sha256.empty()) return;
  Sha256Digest digest = sha256(blob.data, blob.size);
  check_digest(part.name, "operation data", op.data_sha256, digest.data(),
               digest.size());
}

// -------------------------------------------------------------- operations

struct PayloadView {
  const uint8_t* data;
  size_t size;
  int64_t data_offset;
  uint64_t block_size;
};

Span operation_blob(const PayloadView& view, const Operation& op,
                    const std::string& partition) {
  if (op.data_length > SIZE_MAX) {
    throwf("payload: partition %s: operation data too large", partition.c_str());
  }
  const uint64_t base = static_cast<uint64_t>(view.data_offset);
  if (op.data_offset > UINT64_MAX - base) {
    throwf("payload: partition %s: operation data offset overflows",
           partition.c_str());
  }
  uint64_t start = base + op.data_offset;
  if (start > view.size || op.data_length > view.size - start) {
    throwf("payload: partition %s: operation data out of bounds",
           partition.c_str());
  }
  return Span{view.data + start, static_cast<size_t>(op.data_length)};
}

Compression compression_for(OpType type) {
  switch (type) {
    case OpType::kReplace:
      return Compression::kRaw;
    case OpType::kReplaceBz:
      return Compression::kBzip2;
    case OpType::kReplaceXz:
      return Compression::kXz;
    case OpType::kZstd:
      return Compression::kZstd;
    default:
      return Compression::kRaw;
  }
}

void run_replace(const PayloadView& view, Job& job, const Operation& op) {
  const Partition& part = *job.part;
  const uint64_t bs = view.block_size;
  if (op.dst_extents.empty()) throw Error("payload: missing dst_extents");
  validate_extents(op.dst_extents, bs);
  const uint64_t expected = total_blocks(op.dst_extents) * bs;
  const Span blob = operation_blob(view, op, part.name);
  verify_blob(part, op, blob);

  uint64_t start_block = 0;
  uint64_t span_bytes = 0;
  const bool contiguous =
      extents_span(op.dst_extents, bs, &start_block, &span_bytes);

  if (contiguous && span_bytes == expected && job.out.mapped() && expected > 0) {
    DirectSink sink(job.out.mapped_mutable_data() + start_block * bs, expected);
    decompress(compression_for(op.type), blob, sink);
    if (sink.produced() != expected) {
      throwf("payload: wrote %llu bytes, destination extents expect %llu",
             static_cast<unsigned long long>(sink.produced()),
             static_cast<unsigned long long>(expected));
    }
  } else {
    ExtentWriter writer(job.out, op.dst_extents, bs);
    auto& buffer = thread_scratch();
    ScatterSink sink(writer, buffer);
    decompress(compression_for(op.type), blob, sink);
    if (sink.produced() != expected) {
      throwf("payload: wrote %llu bytes, destination extents expect %llu",
             static_cast<unsigned long long>(sink.produced()),
             static_cast<unsigned long long>(expected));
    }
  }
}

void run_zero(Job& job, const Operation& op, uint64_t bs) {
  if (op.dst_extents.empty()) throw Error("payload: missing dst_extents");
  validate_extents(op.dst_extents, bs);
  // Freshly truncated files read as zero. Preflight forbids overlapping
  // destinations, so no other operation can have written these ranges.
  if (!job.has_source) return;
  if (job.out.mapped()) {
    for (const auto& e : op.dst_extents) {
      if (e.num_blocks == 0) continue;
      memset(job.out.mapped_mutable_data() + e.start_block * bs, 0,
             static_cast<size_t>(e.num_blocks * bs));
    }
    return;
  }
  ExtentWriter writer(job.out, op.dst_extents, bs);
  thread_local std::vector<uint8_t> zeros(1u << 20, 0);
  for (const auto& e : op.dst_extents) {
    uint64_t left = e.num_blocks * bs;
    while (left > 0) {
      size_t n = static_cast<size_t>(std::min<uint64_t>(left, zeros.size()));
      writer.write(zeros.data(), n);
      left -= n;
    }
  }
}

// Reads source extents, hashing if requested. Returns a pointer either into the
// source mapping (contiguous case) or into `gathered` (scattered case).
const uint8_t* read_source_span(Job& job, const Operation& op, uint64_t bs,
                                uint64_t bytes, std::vector<uint8_t>& gathered,
                                bool verify, const std::string& partition) {
  if (!job.has_source) {
    throwf("payload: partition %s requires source image data", partition.c_str());
  }
  validate_extents(op.src_extents, bs);
  uint64_t start_block = 0;
  uint64_t span_bytes = 0;
  const bool contiguous = extents_span(op.src_extents, bs, &start_block, &span_bytes);
  const uint8_t* ptr = nullptr;
  if (contiguous && span_bytes == bytes) {
    uint64_t off = start_block * bs;
    if (off > job.source.size() || bytes > job.source.size() - off) {
      throwf("payload: partition %s: source image is too small", partition.c_str());
    }
    ptr = job.source.data() + off;
    if (verify && !op.src_sha256.empty()) {
      Sha256Digest digest = sha256(ptr, static_cast<size_t>(bytes));
      check_digest(partition, "source data", op.src_sha256, digest.data(),
                   digest.size());
    }
  } else {
    gathered.resize(static_cast<size_t>(bytes));
    read_extents(job.source.data(), job.source.size(), op.src_extents, bs,
                 gathered.data(), bytes);
    ptr = gathered.data();
    if (verify && !op.src_sha256.empty()) {
      Sha256Digest digest = sha256(ptr, static_cast<size_t>(bytes));
      check_digest(partition, "source data", op.src_sha256, digest.data(),
                   digest.size());
    }
  }
  return ptr;
}

void run_source_copy(const PayloadView& view, Job& job, const Operation& op,
                     bool skip_verify) {
  const Partition& part = *job.part;
  const uint64_t bs = view.block_size;
  if (op.src_extents.empty() || op.dst_extents.empty()) {
    throw Error("payload: missing src_extents or dst_extents");
  }
  validate_extents(op.dst_extents, bs);
  const uint64_t src_bytes = total_blocks(op.src_extents) * bs;
  const uint64_t dst_bytes = total_blocks(op.dst_extents) * bs;
  if (src_bytes != dst_bytes) {
    throwf("payload: source span (%llu bytes) does not match destination span "
           "(%llu bytes)",
           static_cast<unsigned long long>(src_bytes),
           static_cast<unsigned long long>(dst_bytes));
  }
  std::vector<uint8_t> gathered;
  const uint8_t* src = read_source_span(job, op, bs, src_bytes, gathered,
                                        !skip_verify, part.name);
  uint64_t start_block = 0;
  uint64_t span_bytes = 0;
  if (extents_span(op.dst_extents, bs, &start_block, &span_bytes) &&
      span_bytes == dst_bytes) {
    job.out.write_at(start_block * bs, src, static_cast<size_t>(dst_bytes));
  } else {
    ExtentWriter writer(job.out, op.dst_extents, bs);
    writer.write(src, static_cast<size_t>(dst_bytes));
  }
}

void run_bsdiff(const PayloadView& view, Job& job, const Operation& op,
                bool skip_verify) {
  const Partition& part = *job.part;
  const uint64_t bs = view.block_size;
  if (op.src_extents.empty() || op.dst_extents.empty()) {
    throw Error("payload: missing src_extents or dst_extents");
  }
  validate_extents(op.dst_extents, bs);
  const uint64_t src_bytes = total_blocks(op.src_extents) * bs;
  const uint64_t dst_bytes = total_blocks(op.dst_extents) * bs;
  std::vector<uint8_t> gathered;
  const uint8_t* old = read_source_span(job, op, bs, src_bytes, gathered,
                                        !skip_verify, part.name);
  const Span patch = operation_blob(view, op, part.name);
  verify_blob(part, op, patch);

  thread_local std::vector<uint8_t> patched;
  apply_bsdiff(old, static_cast<size_t>(src_bytes), patch, dst_bytes, patched);
  if (patched.size() != dst_bytes) {
    throwf("bspatch: output size %zu does not match destination span %llu",
           patched.size(), static_cast<unsigned long long>(dst_bytes));
  }
  uint64_t start_block = 0;
  uint64_t span_bytes = 0;
  if (extents_span(op.dst_extents, bs, &start_block, &span_bytes) &&
      span_bytes == dst_bytes) {
    job.out.write_at(start_block * bs, patched.data(), patched.size());
  } else {
    ExtentWriter writer(job.out, op.dst_extents, bs);
    writer.write(patched.data(), patched.size());
  }
}

void run_operation(const PayloadView& view, Job& job, const Operation& op,
                   bool skip_verify) {
  switch (op.type) {
    case OpType::kReplace:
    case OpType::kReplaceBz:
    case OpType::kReplaceXz:
    case OpType::kZstd:
      run_replace(view, job, op);
      break;
    case OpType::kZero:
    case OpType::kDiscard:
      run_zero(job, op, view.block_size);
      break;
    case OpType::kMove:
    case OpType::kSourceCopy:
      run_source_copy(view, job, op, skip_verify);
      break;
    case OpType::kBsdiff:
    case OpType::kSourceBsdiff:
    case OpType::kBrotliBsdiff:
      run_bsdiff(view, job, op, skip_verify);
      break;
    default:
      throwf("payload: unsupported operation %s for partition %s",
             op_type_name(op.type), job.part->name.c_str());
  }
}

// -------------------------------------------------------------- partition IO

void copy_fallthrough(ThreadPool& pool, Job& job, const PayloadView& view) {
  if (!job.has_source) return;
  const Partition& part = *job.part;
  const uint64_t bs = view.block_size;
  if (part.new_size == 0) return;
  const uint64_t total = (part.new_size + bs - 1) / bs;
  Extents covered;
  for (const auto& op : part.operations) {
    covered.insert(covered.end(), op.dst_extents.begin(), op.dst_extents.end());
  }
  validate_extents(covered, bs);
  std::vector<BlockRange> gaps = uncovered_ranges(covered, total);
  if (gaps.empty()) return;

  std::atomic<bool> failed{false};
  std::mutex error_mu;
  std::string error;
  pool.parallel_for(gaps.size(), [&](size_t i) {
    if (failed.load(std::memory_order_relaxed)) return;
    if (stop_requested()) {
      std::lock_guard<std::mutex> lock(error_mu);
      if (!failed.load()) {
        error = "interrupted";
        failed.store(true);
      }
      return;
    }
    const BlockRange& gap = gaps[i];
    uint64_t begin = gap.start * bs;
    uint64_t end = std::min<uint64_t>(gap.end * bs, part.new_size);
    if (begin >= job.source.size()) return;
    end = std::min<uint64_t>(end, job.source.size());
    if (end <= begin) return;
    try {
      job.out.write_at(begin, job.source.data() + begin,
                       static_cast<size_t>(end - begin));
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lock(error_mu);
      if (!failed.load()) {
        error = e.what();
        failed.store(true);
      }
    }
  });
  if (failed.load()) throw Error(error);
}

void verify_source_image(Job& job, bool skip_verify) {
  if (skip_verify || !job.has_source) return;
  const Partition& part = *job.part;
  if (!needs_full_source_verification(part)) return;
  if (part.old_hash.empty()) return;
  uint64_t size = part.old_size;
  if (size > job.source.size()) {
    throwf("payload: partition %s: source image is smaller than expected",
           part.name.c_str());
  }
  Sha256Digest digest = sha256(job.source.data(), static_cast<size_t>(size));
  check_digest(part.name, "source image", part.old_hash, digest.data(),
               digest.size());
}

Sha256Digest hash_output(const OutFile& out, uint64_t size) {
  if (const uint8_t* data = out.mapped_data()) {
    return sha256(data, static_cast<size_t>(size));
  }
  Sha256Stream stream;
  std::vector<uint8_t> buffer(1u << 20);
  uint64_t off = 0;
  while (off < size) {
    size_t n = static_cast<size_t>(std::min<uint64_t>(size - off, buffer.size()));
    out.read_at(off, buffer.data(), n);
    stream.update(buffer.data(), n);
    off += n;
  }
  return stream.finish();
}

void verify_output(Job& job, bool skip_verify) {
  const Partition& part = *job.part;
  if (skip_verify || part.new_hash.empty()) return;
  Sha256Digest digest = hash_output(job.out, part.new_size);
  check_digest(part.name, "output image", part.new_hash, digest.data(),
               digest.size());
}

void cleanup_jobs(std::vector<std::unique_ptr<Job>>& jobs) {
  for (auto& job : jobs) {
    if (!job) continue;
    job->out.finish();
    std::error_code ec;
    std::filesystem::remove(job->out.path(), ec);
  }
}

}  // namespace

// ---------------------------------------------------------------- Payload

const char* op_type_name(OpType type) {
  switch (type) {
    case OpType::kReplace: return "REPLACE";
    case OpType::kReplaceBz: return "REPLACE_BZ";
    case OpType::kMove: return "MOVE";
    case OpType::kBsdiff: return "BSDIFF";
    case OpType::kSourceCopy: return "SOURCE_COPY";
    case OpType::kSourceBsdiff: return "SOURCE_BSDIFF";
    case OpType::kZero: return "ZERO";
    case OpType::kDiscard: return "DISCARD";
    case OpType::kReplaceXz: return "REPLACE_XZ";
    case OpType::kPuffdiff: return "PUFFDIFF";
    case OpType::kBrotliBsdiff: return "BROTLI_BSDIFF";
    case OpType::kZucchini: return "ZUCCHINI";
    case OpType::kLz4diffBsdiff: return "LZ4DIFF_BSDIFF";
    case OpType::kLz4diffPuffdiff: return "LZ4DIFF_PUFFDIFF";
    case OpType::kZstd: return "ZSTD";
  }
  return "UNKNOWN";
}

Payload::~Payload() = default;

std::unique_ptr<Payload> Payload::open(const std::string& path) {
  auto payload = std::unique_ptr<Payload>(new Payload());
  payload->file_ = MappedFile::open(path);
  if (payload->file_.size() < 4) {
    throw Error("payload: input is too small to be a payload.bin");
  }

  const uint8_t* data = payload->file_.data();
  size_t size = payload->file_.size();
  if (memcmp(data, "CrAU", 4) == 0) {
    payload->data_ = data;
    payload->size_ = size;
  } else if (looks_like_zip(data, size)) {
    ZipArchive zip(data, size);
    const ZipEntry* entry = zip.find_payload();
    if (entry == nullptr) {
      throw Error("payload: zip archive does not contain payload.bin");
    }
    if (entry->method == 0) {
      payload->data_ = data + entry->data_offset;
      payload->size_ = static_cast<size_t>(entry->uncompressed_size);
    } else if (entry->method == 8) {
      payload->file_ = zip.read_entry(*entry);
      payload->data_ = payload->file_.data();
      payload->size_ = payload->file_.size();
    } else {
      throwf("payload: unsupported zip compression method %u", entry->method);
    }
  } else {
    throw Error(
        "payload: input is neither a payload.bin (magic \"CrAU\") nor a zip "
        "archive containing payload.bin");
  }

  if (payload->size_ < kHeaderLen) {
    throw Error("payload: truncated header");
  }
  const uint8_t* header = payload->data_;
  payload->header_.version = rd64be(header + 4);
  payload->header_.manifest_len = rd64be(header + 12);
  payload->header_.metadata_signature_len = rd32be(header + 20);
  if (payload->header_.version != 2) {
    throwf("payload: unsupported payload major version %llu (only version 2 "
           "is supported)",
           static_cast<unsigned long long>(payload->header_.version));
  }
  if (payload->header_.manifest_len == 0) {
    throw Error("payload: manifest length is zero");
  }
  const uint64_t metadata_len = kHeaderLen + payload->header_.manifest_len +
                                payload->header_.metadata_signature_len;
  if (payload->header_.manifest_len > INT_MAX ||
      metadata_len > payload->size_) {
    throw Error("payload: corrupt metadata sizes");
  }
  payload->data_offset_ = static_cast<int64_t>(metadata_len);

  chromeos_update_engine::DeltaArchiveManifest manifest;
  if (!manifest.ParseFromArray(payload->data_ + kHeaderLen,
                               static_cast<int>(payload->header_.manifest_len))) {
    throw Error("payload: failed to parse the update manifest");
  }
  payload->block_size_ = manifest.block_size();
  if (payload->block_size_ == 0) payload->block_size_ = 4096;
  if (payload->block_size_ > (1u << 20)) {
    throwf("payload: unreasonable block size %llu",
           static_cast<unsigned long long>(payload->block_size_));
  }
  payload->minor_version_ = manifest.minor_version();
  payload->partial_update_ = manifest.partial_update();

  payload->partitions_.reserve(static_cast<size_t>(manifest.partitions_size()));
  for (int i = 0; i < manifest.partitions_size(); ++i) {
    const auto& src = manifest.partitions(i);
    Partition part;
    part.name = src.partition_name();
    if (part.name.empty()) throw Error("payload: partition with empty name");
    if (part.name.find('/') != std::string::npos ||
        part.name.find("..") != std::string::npos) {
      throwf("payload: refusing unsafe partition name %s", part.name.c_str());
    }
    if (src.has_new_partition_info()) {
      part.new_size = src.new_partition_info().size();
      part.new_hash = to_bytes(src.new_partition_info().hash());
    }
    if (src.has_old_partition_info()) {
      part.old_size = src.old_partition_info().size();
      part.old_hash = to_bytes(src.old_partition_info().hash());
    }
    part.operations.reserve(static_cast<size_t>(src.operations_size()));
    bool unsupported_seen[64] = {false};
    for (int j = 0; j < src.operations_size(); ++j) {
      const auto& src_op = src.operations(j);
      Operation op;
      op.type = static_cast<OpType>(src_op.type());
      op.data_offset = src_op.data_offset();
      op.data_length = src_op.data_length();
      op.src_length = src_op.src_length();
      op.dst_length = src_op.dst_length();
      for (int k = 0; k < src_op.src_extents_size(); ++k) {
        add_extent(op.src_extents, src_op.src_extents(k));
      }
      for (int k = 0; k < src_op.dst_extents_size(); ++k) {
        add_extent(op.dst_extents, src_op.dst_extents(k));
      }
      op.data_sha256 = to_bytes(src_op.data_sha256_hash());
      op.src_sha256 = to_bytes(src_op.src_sha256_hash());
      if (operation_needs_source(op)) part.needs_source = true;
      if (!operation_supported(op.type)) {
        int idx = static_cast<int>(op.type);
        if (idx >= 0 && idx < 64 && !unsupported_seen[idx]) {
          unsupported_seen[idx] = true;
          part.unsupported_ops.emplace_back(op_type_name(op.type));
        }
      }
      part.operations.push_back(std::move(op));
    }

    if (src.has_hash_tree_data_extent()) {
      part.hash_tree.has_data = true;
      add_extent(part.hash_tree.data_extent, src.hash_tree_data_extent());
    }
    if (src.has_hash_tree_extent()) {
      part.hash_tree.has_tree = true;
      add_extent(part.hash_tree.tree_extent, src.hash_tree_extent());
    }
    if (src.has_hash_tree_algorithm()) {
      part.hash_tree.algorithm = src.hash_tree_algorithm();
    }
    part.hash_tree.salt = to_bytes(src.hash_tree_salt());
    if (src.has_fec_data_extent()) {
      part.fec.has_data = true;
      add_extent(part.fec.data_extent, src.fec_data_extent());
    }
    if (src.has_fec_extent()) {
      part.fec.has_fec = true;
      add_extent(part.fec.fec_extent, src.fec_extent());
    }
    part.fec.roots = src.fec_roots();

    payload->partitions_.push_back(std::move(part));
  }
  return payload;
}

bool Payload::is_delta() const {
  if (minor_version_ != 0 || partial_update_) return true;
  for (const auto& part : partitions_) {
    if (part.needs_source) return true;
  }
  return false;
}

std::vector<const Partition*> Payload::select(
    const std::vector<std::string>& names) const {
  std::vector<const Partition*> out;
  if (names.empty()) {
    for (const auto& part : partitions_) out.push_back(&part);
    return out;
  }
  std::vector<std::string> unknown;
  for (const auto& name : names) {
    const Partition* found = nullptr;
    for (const auto& part : partitions_) {
      if (part.name == name) {
        found = &part;
        break;
      }
    }
    if (found == nullptr) {
      unknown.push_back(name);
    } else {
      out.push_back(found);
    }
  }
  if (!unknown.empty()) {
    throwf("payload: requested partitions not present in payload: %s",
           join(unknown, ", ").c_str());
  }
  return out;
}

void Payload::extract(const ExtractOptions& options) {
  using ProfileClock = std::chrono::steady_clock;
  const bool profile = std::getenv("PD_PROFILE") != nullptr;
  const auto profile_start = ProfileClock::now();
  std::array<std::atomic<uint64_t>, 15> operation_ns{};
  std::atomic<uint64_t> verification_ns{0};
  auto timed_verify = [&](Job& job, bool skip) {
    const auto start = profile ? ProfileClock::now() : ProfileClock::time_point{};
    verify_output(job, skip);
    if (profile) verification_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(ProfileClock::now() - start).count(), std::memory_order_relaxed);
  };
  std::vector<const Partition*> selected = select(options.partitions);

  // Direct mmap writes bypass OutFile::write_at: validate their complete
  // address range and independence before any output is created or scheduled.
  for (const Partition* part : selected) {
    std::vector<BlockRange> destinations;
    for (const auto& op : part->operations) {
      validate_extents(op.dst_extents, block_size_);
      for (const auto& extent : op.dst_extents) {
        if (!extent.num_blocks) continue;
        if (extent.start_block + extent.num_blocks > part->new_size / block_size_) {
          throwf("payload: destination extent exceeds partition %s", part->name.c_str());
        }
        destinations.push_back({extent.start_block, extent.start_block + extent.num_blocks});
      }
    }
    std::sort(destinations.begin(), destinations.end(),
              [](const BlockRange& a, const BlockRange& b) { return a.start < b.start; });
    for (size_t i = 1; i < destinations.size(); ++i) {
      if (destinations[i].start < destinations[i - 1].end) {
        throwf("payload: overlapping destination extents in partition %s", part->name.c_str());
      }
    }
  }

  std::vector<std::string> unsupported;
  for (const Partition* part : selected) {
    if (!part->unsupported_ops.empty()) {
      unsupported.push_back(part->name + " (" + join(part->unsupported_ops, ", ") + ")");
    }
  }
  if (!unsupported.empty()) {
    throwf("payload: partitions use install operations that are not supported "
           "yet: %s",
           join(unsupported, "; ").c_str());
  }

  if (options.source_dir.empty()) {
    std::vector<std::string> missing;
    for (const Partition* part : selected) {
      if (part->needs_source) missing.push_back(part->name);
    }
    if (!missing.empty()) {
      throwf("payload: this is a delta (incremental) payload; source images are "
             "required for: %s",
             join(missing, ", ").c_str());
    }
  }

  std::error_code ec;
  if (!options.output_dir.empty()) {
    std::filesystem::create_directories(options.output_dir, ec);
    if (ec) {
      throwf("payload: cannot create output directory %s: %s",
             options.output_dir.c_str(), ec.message().c_str());
    }
  }

  int concurrency = options.concurrency;
  if (concurrency <= 0) {
    concurrency = static_cast<int>(std::thread::hardware_concurrency());
    if (concurrency <= 0) concurrency = 1;
  }
  const uint64_t bs = block_size_;
  const PayloadView view{data_, size_, data_offset_, bs};

  std::vector<std::unique_ptr<Job>> jobs;
  jobs.reserve(selected.size());
  try {
    for (const Partition* part : selected) {
      auto job = std::make_unique<Job>();
      job->part = part;
      const std::string out_path =
          (std::filesystem::path(options.output_dir) / (part->name + ".img"))
              .string();
      job->out = OutFile::create(out_path, part->new_size);
      if (part->needs_source) {
        const std::string src_path =
            (std::filesystem::path(options.source_dir) / (part->name + ".img"))
                .string();
        job->source = MappedFile::open(src_path);
        job->has_source = true;
        if (job->source.size() < part->old_size) {
          throwf("payload: source image %s is smaller than expected (%llu < "
                 "%llu bytes); wrong or truncated source for partition %s",
                 src_path.c_str(),
                 static_cast<unsigned long long>(job->source.size()),
                 static_cast<unsigned long long>(part->old_size),
                 part->name.c_str());
        }
      }
      jobs.push_back(std::move(job));
    }
  } catch (...) {
    cleanup_jobs(jobs);
    throw;
  }

  std::atomic<bool> cancel{false};
  // Declare verification first so extraction workers are joined first during
  // unwinding; verification tasks can outlive their submitting worker.
  ThreadPool verify_pool(static_cast<size_t>(std::max(1, concurrency / 2)));
  ThreadPool pool(static_cast<size_t>(concurrency));
  auto enqueue_verification = [&](Job* job, bool skip) {
    verify_pool.submit([&, job, skip] {
      guarded(*job, [&] { timed_verify(*job, skip); });
      job->output_verified = !job->failed.load();
      if (job->failed.load()) cancel.store(true, std::memory_order_relaxed);
    });
  };

  // Phase 0: verify whole source images that lack per-operation hashes, one
  // task per partition (sequential hashing inside each task).
  for (auto& job : jobs) {
    if (!job->has_source || !needs_full_source_verification(*job->part)) continue;
    Job* j = job.get();
    verify_pool.submit([j, skip = options.skip_verify] {
      guarded(*j, [j, skip] { verify_source_image(*j, skip); });
    });
  }
  verify_pool.wait();

  for (auto& job : jobs) {
    if (job->failed.load()) {
      std::string message = get_error(*job);
      cleanup_jobs(jobs);
      throwf("payload: partition %s: %s", job->part->name.c_str(),
             message.c_str());
    }
  }

  // Phase 1a: copy blocks that the delta payload leaves untouched.
  for (auto& job : jobs) {
    if (!job->has_source) continue;
    try {
      copy_fallthrough(pool, *job, view);
    } catch (const std::exception& e) {
      cleanup_jobs(jobs);
      throwf("payload: partition %s: %s", job->part->name.c_str(), e.what());
    }
  }

  // Phase 1b: apply install operations concurrently. Operations read the
  // immutable source image and write disjoint destination extents, so they are
  // safe to run in parallel.
  const auto operations_start = ProfileClock::now();
  for (auto& job : jobs) {
    const int total = static_cast<int>(job->part->operations.size());
    job->remaining.store(total);
    if (options.progress != nullptr) {
      ProgressEvent event;
      event.partition = &job->part->name;
      event.total_ops = total;
      options.progress->on_progress(event);
    }
  }
  auto execute = [&](Job* j, size_t i) {
    if (cancel.load(std::memory_order_relaxed) || stop_requested()) return;
    const Operation& op = j->part->operations[i];
    const auto op_start = profile ? ProfileClock::now() : ProfileClock::time_point{};
    guarded(*j, [&] { run_operation(view, *j, op, options.skip_verify); });
    if (profile) operation_ns[static_cast<size_t>(op.type)].fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(ProfileClock::now() - op_start).count(), std::memory_order_relaxed);
    if (j->failed.load(std::memory_order_relaxed)) {
      cancel.store(true, std::memory_order_relaxed);
    }
    int completed = j->completed.fetch_add(1) + 1;
    if (options.progress != nullptr && !j->failed.load()) {
      ProgressEvent event;
      event.partition = &j->part->name;
      event.total_ops = static_cast<int>(j->part->operations.size());
      event.completed_ops = completed;
      options.progress->on_progress(event);
    }
    if (completed == static_cast<int>(j->part->operations.size())) {
      j->remaining.store(0);
      // The acq_rel completion counter publishes every preceding output write.
      // Publish the completed partition to the dedicated verification pool;
      // this extraction worker immediately resumes taking operations.
      if (!j->failed.load() && !j->part->hash_tree.has_tree && !j->part->fec.has_fec) {
        enqueue_verification(j, options.skip_verify);
      }
    }
  };

  // Keep the scheduler alive until all long-lived worker tasks have returned.
  std::vector<OperationScheduler::PartitionWork> work;
  for (const auto& job : jobs) {
    OperationScheduler::PartitionWork part_work{job->part->new_size, job->part->operations.size()};
    if (concurrency >= 4) {
      for (const auto& op : job->part->operations) {
        uint64_t bytes = total_blocks(op.dst_extents) * bs;
        // Coarse cost classes, not a prediction in seconds. Stable ties keep
        // manifest locality; expensive/large operations start first.
        uint64_t weight = 4;
        if (op.type == OpType::kZero || op.type == OpType::kDiscard) weight = 0;
        else if (op.type == OpType::kReplace || op.type == OpType::kSourceCopy || op.type == OpType::kMove) weight = 1;
        else if (op.type == OpType::kZstd) weight = 2;
        part_work.costs.push_back(weight && bytes > UINT64_MAX / weight ? UINT64_MAX : bytes * weight);
      }
    }
    work.push_back(std::move(part_work));
  }
  OperationScheduler scheduler(work);
  if (concurrency >= 4) {
    const int large_workers = concurrency / 4;
    for (int worker = 0; worker < concurrency; ++worker) {
      pool.submit([&, largest_first = worker < large_workers] {
        while (!cancel.load(std::memory_order_relaxed) && !stop_requested()) {
          auto task = scheduler.take(largest_first);
          if (!task) break;
          execute(jobs[task->first].get(), task->second);
        }
      });
    }
  } else {
    // Preserve manifest-order FIFO for fewer than four workers.
    for (auto& job : jobs) {
      for (size_t i = 0; i < job->part->operations.size(); ++i) {
        pool.submit([&, j = job.get(), i] { execute(j, i); });
      }
    }
  }
  pool.wait();
  verify_pool.wait();

  if (stop_requested()) {
    cleanup_jobs(jobs);
    throw Error("payload: interrupted");
  }

  std::string first_error;
  for (auto& job : jobs) {
    if (job->failed.load()) {
      if (first_error.empty()) {
        first_error = "payload: partition " + job->part->name + ": " +
                      get_error(*job);
      }
    }
  }
  if (!first_error.empty()) {
    cleanup_jobs(jobs);
    throw Error(first_error);
  }

  const auto operations_end = ProfileClock::now();

  // Phase 2: regenerate dm-verity metadata sequentially (each partition uses
  // the extraction pool internally), publishing each ready output to vfy workers.
  for (auto& job : jobs) {
    const Partition& part = *job->part;
    try {
      job->fec_skipped = write_verity(
          pool, job->out, part, bs, options.no_fec, [&](int done, int total) {
            if (options.progress != nullptr) {
              ProgressEvent event;
              event.partition = &part.name;
              event.total_ops = static_cast<int>(part.operations.size());
              event.completed_ops = event.total_ops;
              event.fec_phase = true;
              event.phase_completed = done;
              event.phase_total = total;
              options.progress->on_progress(event);
            }
          });
    } catch (const std::exception& e) {
      verify_pool.wait();
      cleanup_jobs(jobs);
      throwf("payload: partition %s: %s", part.name.c_str(), e.what());
    }
    if (!job->output_verified) {
      enqueue_verification(job.get(), options.skip_verify || job->fec_skipped);
    }
    if (job->fec_skipped) {
      fprintf(stderr,
              "Warning: skipped FEC generation for partition %s; image does "
              "not match its expected sha256 and must not be flashed\n",
              part.name.c_str());
    }
  }

  verify_pool.wait();

  for (auto& job : jobs) {
    if (job->failed.load()) {
      if (first_error.empty()) {
        first_error = "payload: partition " + job->part->name + ": " +
                      get_error(*job);
      }
    }
  }
  if (!first_error.empty()) {
    cleanup_jobs(jobs);
    throw Error(first_error);
  }

  for (auto& job : jobs) {
    if (!job->out.finish()) {
      throwf("payload: I/O error while finishing %s",
             job->out.path().c_str());
    }
    if (options.progress != nullptr) {
      ProgressEvent event;
      event.partition = &job->part->name;
      event.total_ops = static_cast<int>(job->part->operations.size());
      event.completed_ops = event.total_ops;
      event.done = true;
      options.progress->on_progress(event);
    }
  }
  if (profile) {
    auto seconds = [](auto duration) { return std::chrono::duration<double>(duration).count(); };
    fprintf(stderr, "profile extraction: total_wall_s=%.6f setup_wall_s=%.6f operations_and_early_verify_wall_s=%.6f finalization_wall_s=%.6f verify_worker_s=%.6f\n",
            seconds(ProfileClock::now() - profile_start), seconds(operations_start - profile_start),
            seconds(operations_end - operations_start), seconds(ProfileClock::now() - operations_end),
            verification_ns.load() / 1e9);
    for (size_t i = 0; i < operation_ns.size(); ++i) {
      if (operation_ns[i].load()) fprintf(stderr, "profile operation: type=%s worker_s=%.6f\n", op_type_name(static_cast<OpType>(i)), operation_ns[i].load() / 1e9);
    }
  }

}

}  // namespace pd
