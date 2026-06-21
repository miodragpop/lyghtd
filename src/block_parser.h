#pragma once

// Block/transaction parser — ports the Go lightwalletd parser (parser/block.go,
// block_header.go, transaction.go) to build a CompactBlock from a full block's
// raw bytes. Sapling (V4) and the pre-Sapling Zcash history (Sprout/Overwinter)
// are fully handled; the V5/Orchard (ZIP225/NU5) path is ported but gated by the
// NU5 activation height (so on a chain without NU5 — e.g. Ycash — it is off).
//
// The parser computes everything itself from the raw bytes: the block hash and
// per-tx txids via SHA256d, prevHash + time from the header, and the commitment
// tree sizes via running cumulative output/action counts. (Txids are SHA256d of
// the tx bytes, which is exact for every non-V5 transaction; V5 txids differ, so
// below the NU5 height the caller may instead pass txids from a verbose getblock
// via `txids_display`.) This lets the ingestor drop the verbose RPC call.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "compact_formats.pb.h"

namespace lyghtd {

namespace rpc = cash::z::wallet::sdk::rpc;

// Set the NU5 (ZIP225/Orchard) activation height. V5 transaction parsing is
// enabled only for blocks at/above this height. Default is "never" (UINT64_MAX),
// i.e. no NU5 — correct for Ycash. Set once at startup from the daemon.
void SetNU5ActivationHeight(uint64_t height);

// Parse a standalone transaction (e.g. from the mempool) into a CompactTx.
// `wire_txid` is the 32-byte txid in wire (little-endian) order, stored as-is
// (matching Go's mempool path, which takes the txid from getrawmempool rather
// than recomputing it). Index is 0, so transparent inputs are omitted — exactly
// like Go's tx.ToCompact(0). V5/Orchard parsing follows the configured NU5
// height (the mempool sits at the tip). Throws on malformed/trailing data.
rpc::CompactTx ParseTransactionToCompact(std::string_view raw_tx,
                                         const std::string& wire_txid);

// Parse a full block into its CompactBlock. Throws std::runtime_error on any
// malformed/short data or an unexpected height/txcount mismatch.
//
//   raw                 : full block bytes (decoded from getblock <hash> 0)
//   height              : the block height being ingested
//   sapling_tree_size   : IN cumulative Sapling note count through the PREVIOUS
//                         block; OUT the count through THIS block (= this block's
//                         chainMetadata.saplingCommitmentTreeSize).
//   orchard_tree_size   : same, for Orchard actions.
//   txids_display       : optional. If non-null, per-tx txids in big-endian
//                         display hex (from a verbose getblock) are used instead
//                         of being computed — needed for V5 txs. Null elsewhere.
rpc::CompactBlock ParseBlockToCompact(
    std::string_view raw, uint64_t height, uint32_t& sapling_tree_size,
    uint32_t& orchard_tree_size,
    const std::vector<std::string>* txids_display = nullptr);

}  // namespace lyghtd
