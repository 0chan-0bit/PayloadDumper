#pragma once

#include <cstdint>
#include <functional>

#include "payload.h"
#include "pool.h"

namespace pd {

class OutFile;

// Rebuilds the dm-verity hash tree and (unless no_fec) the FEC parity for a
// partition. Returns true when FEC generation was explicitly skipped.
bool write_verity(ThreadPool& pool, OutFile& out, const Partition& part,
                  uint64_t block_size, bool no_fec,
                  const std::function<void(int, int)>& fec_progress);

}  // namespace pd
