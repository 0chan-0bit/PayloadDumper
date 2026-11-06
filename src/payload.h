#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "extents.h"
#include "progress.h"
#include "util.h"

namespace pd {

struct Header {
  uint64_t version = 0;
  uint64_t manifest_len = 0;
  uint32_t metadata_signature_len = 0;
};

enum class OpType : int {
  kReplace = 0,
  kReplaceBz = 1,
  kMove = 2,
  kBsdiff = 3,
  kSourceCopy = 4,
  kSourceBsdiff = 5,
  kZero = 6,
  kDiscard = 7,
  kReplaceXz = 8,
  kPuffdiff = 9,
  kBrotliBsdiff = 10,
  kZucchini = 11,
  kLz4diffBsdiff = 12,
  kLz4diffPuffdiff = 13,
  kZstd = 14,
};

const char* op_type_name(OpType type);

struct Operation {
  OpType type = OpType::kReplace;
  uint64_t data_offset = 0;
  uint64_t data_length = 0;
  uint64_t src_length = 0;
  uint64_t dst_length = 0;
  Extents src_extents;
  Extents dst_extents;
  std::vector<uint8_t> data_sha256;
  std::vector<uint8_t> src_sha256;
};

struct HashTreeInfo {
  bool has_tree = false;
  bool has_data = false;
  Extents data_extent;
  Extents tree_extent;
  std::string algorithm = "sha1";
  std::vector<uint8_t> salt;
};

struct FecInfo {
  bool has_fec = false;
  bool has_data = false;
  Extents data_extent;
  Extents fec_extent;
  uint32_t roots = 2;
};

struct Partition {
  std::string name;
  uint64_t new_size = 0;
  uint64_t old_size = 0;
  std::vector<uint8_t> new_hash;
  std::vector<uint8_t> old_hash;
  bool needs_source = false;
  std::vector<std::string> unsupported_ops;
  std::vector<Operation> operations;
  HashTreeInfo hash_tree;
  FecInfo fec;
};

struct ExtractOptions {
  std::string output_dir;
  std::vector<std::string> partitions;
  std::string source_dir;
  int concurrency = 0;
  bool skip_verify = false;
  bool no_fec = false;
  ProgressSink* progress = nullptr;
  bool quiet = false;
};

class Payload {
 public:
  static std::unique_ptr<Payload> open(const std::string& path);
  ~Payload();

  const Header& header() const { return header_; }
  uint32_t minor_version() const { return minor_version_; }
  bool partial_update() const { return partial_update_; }
  uint64_t block_size() const { return block_size_; }
  bool is_delta() const;
  const std::vector<Partition>& partitions() const { return partitions_; }

  void extract(const ExtractOptions& options);

 private:
  Payload() = default;
  void parse_manifest();
  std::vector<const Partition*> select(const std::vector<std::string>& names) const;

  MappedFile file_;
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  int64_t data_offset_ = 0;
  Header header_;
  std::vector<Partition> partitions_;
  uint64_t block_size_ = 4096;
  uint32_t minor_version_ = 0;
  bool partial_update_ = false;
};

}  // namespace pd
