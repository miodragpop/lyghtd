#pragma once

// BlockCache — read-only view of the on-disk lightwalletd cache, byte-compatible
// with the Go implementation (common/cache.go). Two parallel append-only files
// under <data-dir>/db/<chain>/:
//
//   lengths : array of uint32 LE, one per block from height 0. lengths[h] = N,
//             the protobuf byte length of block h (EXCLUDING the 8-byte checksum).
//   blocks  : concatenated records, each = [8-byte checksum][N-byte protobuf].
//             offset(h) = sum over i<h of (lengths[i] + 8).
//
// firstBlock is hardcoded 0 (NOT stored), so array index == block height.
//
// Concurrency mirrors the Go model: one std::shared_mutex guards the starts_
// offset index + block-count bounds. Readers take a shared lock and pread()
// independently from a shared fd (stateless positional reads, zero contention).
// A future ingestor will take the unique lock to append a tip block.

#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "compact_formats.pb.h"

namespace lyghtd {

namespace rpc = cash::z::wallet::sdk::rpc;

class BlockCache {
public:
    // Open the cache at <data_dir>/db/<chain>/. Throws std::runtime_error on
    // any I/O or consistency failure (matches the Go reader's fatal startup).
    // If `writable`, the files are opened O_RDWR|O_CREAT (creating the dir and
    // empty files if absent) so the ingestor can append; otherwise read-only.
    static std::unique_ptr<BlockCache> Open(const std::string& data_dir,
                                            const std::string& chain,
                                            bool writable = false);

    ~BlockCache();

    BlockCache(const BlockCache&) = delete;
    BlockCache& operator=(const BlockCache&) = delete;

    // Number of blocks currently in the cache (== latest height + 1, since
    // firstBlock is 0). Tip height is Count() - 1.
    uint64_t Count() const;

    // Latest (tip) height, or std::nullopt if the cache is empty.
    std::optional<uint64_t> LatestHeight() const;

    // Read and parse the CompactBlock at the given height. Returns nullopt if
    // height is out of range. Throws std::runtime_error on cache corruption
    // (bad checksum, length out of bounds, height mismatch) — same conditions
    // the Go reader treats as corruption.
    std::optional<rpc::CompactBlock> Get(uint64_t height) const;

    const std::string& chain() const { return chain_; }

    // ---- Write path (ingestor; requires the cache opened writable) ----

    // The next height to ingest (== Count(), since firstBlock is 0).
    uint64_t NextHeight() const { return Count(); }

    // Tip block's hash (CompactBlock.hash, wire order), or empty if no blocks.
    std::string LatestHash() const;

    // True if the cache is empty or its tip hash equals `prevhash` — i.e. a
    // block with this prevHash extends the chain (mirrors Go HashMatch).
    bool HashMatch(const std::string& prevhash) const;

    // Append `block` at `height` (must equal NextHeight() and block.height()).
    // Serializes the proto, prepends the FNV checksum, appends to blocks +
    // lengths, and updates the in-memory index under the unique lock. Throws on
    // I/O failure or a height/consistency violation. Requires writable open.
    void Add(uint64_t height, const rpc::CompactBlock& block);

    // Append an already-serialized CompactBlock proto at `height` (the fast path
    // for getcompactblockrange, whose bytes are stored verbatim). Parses the
    // compact proto only to validate height, verify prevHash continuity against
    // the tip, and read the tip hash — the original bytes are written as-is (no
    // reserialize). Throws on a height/continuity/parse violation. The prevHash
    // check makes this self-guarding, so it's used for the reorg-safe deep range.
    void AddRaw(uint64_t height, const std::string& proto);

    // Drop blocks from `height` onward (truncate both files), for reorgs.
    void Reorg(uint64_t height);

    // fsync both files (call after a batch of appends).
    void Sync();

private:
    BlockCache() = default;

    // Reads the raw [checksum||protobuf] record for `height` into `out` (the
    // protobuf bytes only) after verifying the checksum. Caller holds the
    // shared lock and has bounds-checked `height`. Returns false on corruption.
    bool ReadRaw(uint64_t height, std::string& out) const;

    // Frame `data` as [8-byte FNV checksum][data], append to both files, and
    // update the in-memory index + tip hash. Caller holds the unique lock and
    // has verified height == lengths_.size(). Shared by Add() and AddRaw().
    void AppendRecordLocked(uint64_t height, const std::string& data,
                            const std::string& hash);

    std::string chain_;
    int blocks_fd_ = -1;
    int lengths_fd_ = -1;   // only opened when writable
    bool writable_ = false;

    // starts_[h] = byte offset of block h's record (checksum) in `blocks`.
    // starts_ has lengths_.size()+1 entries: a leading 0 and the cumulative end
    // offset after each block (so starts_.back() == total `blocks` file size).
    mutable std::shared_mutex mu_;
    std::vector<uint64_t> starts_;   // guarded by mu_
    std::vector<uint32_t> lengths_;  // protobuf length per block; guarded by mu_
    std::string latest_hash_;        // tip block hash (wire); guarded by mu_
};

// FNV-1a 64-bit over (height as uint64 LE) THEN the protobuf bytes — exactly
// the Go `checksum(height, data)` in common/cache.go. Exposed for reuse by the
// future ingestor (M2) and for tests.
uint64_t CacheChecksum(uint64_t height, const std::string& data);

}  // namespace lyghtd
