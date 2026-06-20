#pragma once

// Ingestor — the standard-RPC block source. Fetches raw blocks from ycashd (one
// getblock verbosity-0 call/block below the NU5 height), parses them into
// CompactBlocks — computing the block hash and txids via SHA256d and the
// note-commitment tree sizes via running counters — and appends them to the
// cache. At/above the NU5 height it also fetches verbose getblock for V5 txids.
// Uses a single kept-alive RPC connection.
//
// M3 will refactor "where blocks come from" behind a CompactBlockSource
// interface; for M2 the RPC fetch+parse lives here.

#include <atomic>
#include <cstdint>
#include <optional>

#include "block_cache.h"
#include "rpc_client.h"

namespace lyghtd {

// Running note-commitment tree sizes (cumulative output/action counts). The
// parser advances these per block so the ingestor no longer needs the verbose
// getblock call to learn the tree sizes.
struct TreeState {
    uint32_t sapling = 0;
    uint32_t orchard = 0;
};

// Read the tree sizes through the cache tip (from its stored chainMetadata) to
// seed a TreeState on resume; {0,0} if the cache is empty.
TreeState SeedTreeState(BlockCache& cache);

// Fetch + parse the block at `height` from ycashd into a CompactBlock, advancing
// `ts`. Below `nu5_height` only a raw getblock is fetched (txids computed via
// SHA256d); at/above it, verbose+raw is fetched (V5 txids from verbose). Returns
// nullopt if the daemon doesn't have that height yet (RPC error -8). Throws on
// any other RPC/parse failure.
std::optional<rpc::CompactBlock> GetBlockFromRPC(RpcClient& rpc, uint64_t height,
                                                 TreeState& ts,
                                                 uint64_t nu5_height);

// Sequential catch-up to `target_height` inclusive from NextHeight(), one block
// at a time, handling reorgs. Used by tools. Returns blocks added. Requires a
// writable cache.
uint64_t IngestUpTo(BlockCache& cache, RpcClient& rpc, uint64_t target_height,
                    uint64_t nu5_height);

// Continuous ingestor for the running daemon over a single kept-alive
// connection. Historical blocks (more than 100 below the tip — deeper than the
// Zcash/Ycash max reorg depth, so final) are fetched in batches of `batch_size`
// per request via a producer-consumer pipeline; within the top 100 it switches
// to single-block fetch with reorg handling. Sleeps when synced. Runs until
// `stop`. Requires a writable cache.
//
// When `use_compact`, the deep range is pulled as ready-made CompactBlock protos
// via getcompactblockrange and stored verbatim (ycashd does the block→compact
// work); the top-100 reorg zone still uses the standard parsed path. Callers
// gate this on RpcClient::SupportsCompactBlocks().
void RunIngestor(BlockCache& cache, RpcClient& rpc, std::atomic<bool>& stop,
                 uint64_t batch_size, uint64_t nu5_height, bool use_compact);

}  // namespace lyghtd
