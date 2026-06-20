#include "compact_tx_streamer.h"

#include <algorithm>

#include "Version.h"

namespace lyghtd {

namespace {

// Ycash mainnet constants (Canopy, no NU5/Orchard). These are the
// cache-derivable/static fields of GetLightdInfo; the daemon-only fields
// (estimatedHeight, zcashdBuild/Subversion, pending upgrades) are filled in
// M2 once the ycashd client exists.
constexpr char kChainName[] = "main";
constexpr char kConsensusBranchId[] = "19bd2d2f";  // Canopy chaintip
constexpr uint64_t kSaplingActivationHeight = 419200;

// True if this compact tx carries any shielded (Sapling/Orchard) component.
// Mirrors FilterTxPool's emptiness test for the default (PoolTypes empty)
// case: a tx survives the legacy shielded-only filter iff it has spends,
// outputs, or actions.
bool HasShielded(const rpc::CompactTx& tx) {
    return tx.spends_size() > 0 || tx.outputs_size() > 0 ||
           tx.actions_size() > 0;
}

// Apply the legacy shielded-only filter to a block IN PLACE: drop txs with no
// Sapling/Orchard components, and for surviving txs strip transparent vin/vout
// (PoolTypes empty => only shielded pools requested). Empty blocks and blocks
// whose txs are all filtered are still returned (header-only), matching Go.
void FilterShieldedOnly(rpc::CompactBlock* block) {
    google::protobuf::RepeatedPtrField<rpc::CompactTx> kept;
    for (const auto& tx : block->vtx()) {
        if (!HasShielded(tx)) continue;
        rpc::CompactTx* r = kept.Add();
        r->set_index(tx.index());
        r->set_txid(tx.txid());
        r->set_fee(tx.fee());
        *r->mutable_spends() = tx.spends();
        *r->mutable_outputs() = tx.outputs();
        *r->mutable_actions() = tx.actions();
        // vin/vout intentionally dropped (transparent pool not requested).
    }
    block->mutable_vtx()->Swap(&kept);
}

}  // namespace

grpc::Status CompactTxStreamerImpl::GetLightdInfo(grpc::ServerContext*,
                                                  const rpc::Empty*,
                                                  rpc::LightdInfo* resp) {
    resp->set_version(k_version);
    resp->set_vendor("lyghtd");
    resp->set_taddrsupport(true);
    resp->set_chainname(kChainName);
    resp->set_saplingactivationheight(kSaplingActivationHeight);
    resp->set_consensusbranchid(kConsensusBranchId);
    if (auto tip = cache_->LatestHeight(); tip.has_value()) {
        resp->set_blockheight(*tip);
        // No daemon in M1, so the best estimate of chain height IS the cache
        // tip (cache is at tip when synced).
        resp->set_estimatedheight(*tip);
    }
    return grpc::Status::OK;
}

grpc::Status CompactTxStreamerImpl::GetLatestBlock(grpc::ServerContext*,
                                                   const rpc::ChainSpec*,
                                                   rpc::BlockID* resp) {
    auto tip = cache_->LatestHeight();
    if (!tip.has_value()) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "cache is empty");
    }
    // The tip CompactBlock carries both height and hash (wire/little-endian
    // order, exactly what the Go server returns), so no daemon needed.
    std::optional<rpc::CompactBlock> block;
    try {
        block = cache_->Get(*tip);
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }
    if (!block.has_value()) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "tip not readable");
    }
    resp->set_height(block->height());
    resp->set_hash(block->hash());
    return grpc::Status::OK;
}

grpc::Status CompactTxStreamerImpl::GetBlock(grpc::ServerContext*,
                                             const rpc::BlockID* req,
                                             rpc::CompactBlock* resp) {
    // Proto zero-value (Height==0 && Hash empty) means "unspecified"; reject.
    if (req->height() == 0 && req->hash().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "GetBlock: request for unspecified identifier");
    }
    // Hash is more specific than height, but get-by-hash isn't implemented
    // (matches upstream).
    if (!req->hash().empty()) {
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            "GetBlock: Block hash specifier is not yet implemented");
    }
    std::optional<rpc::CompactBlock> block;
    try {
        block = cache_->Get(req->height());
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
    }
    if (!block.has_value()) {
        return grpc::Status(
            grpc::StatusCode::OUT_OF_RANGE,
            "GetBlock: block " + std::to_string(req->height()) +
                " is newer than the latest block");
    }
    // Single GetBlock does NOT filter — returns all txs as stored.
    *resp = std::move(*block);
    return grpc::Status::OK;
}

grpc::Status CompactTxStreamerImpl::GetBlockRange(
    grpc::ServerContext* ctx, const rpc::BlockRange* req,
    grpc::ServerWriter<rpc::CompactBlock>* writer) {
    if (!req->has_start() || !req->has_end()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "GetBlockRange: must specify start and end heights");
    }
    for (auto pt : req->pooltypes()) {
        if (pt == rpc::POOL_TYPE_INVALID) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "GetBlockRange: invalid pool type requested");
        }
    }
    // Default (no pool types) => legacy shielded-only filtering. M1 supports
    // only that default; a non-empty PoolTypes request would need transparent
    // data handling we add later.
    const bool shielded_only = req->pooltypes_size() == 0;

    const uint64_t start = req->start().height();
    const uint64_t end = req->end().height();
    const bool reversed = start > end;
    const uint64_t low = reversed ? end : start;
    const uint64_t high = reversed ? start : end;

    for (uint64_t i = low; i <= high; ++i) {
        if (ctx->IsCancelled()) {
            return grpc::Status(grpc::StatusCode::CANCELLED, "client cancelled");
        }
        const uint64_t h = reversed ? (high - (i - low)) : i;
        std::optional<rpc::CompactBlock> block;
        try {
            block = cache_->Get(h);
        } catch (const std::exception& e) {
            return grpc::Status(grpc::StatusCode::INTERNAL, e.what());
        }
        if (!block.has_value()) {
            return grpc::Status(
                grpc::StatusCode::OUT_OF_RANGE,
                "GetBlockRange: block " + std::to_string(h) +
                    " is newer than the latest block");
        }
        if (shielded_only) {
            FilterShieldedOnly(&block.value());
        }
        // Blocks with all txs filtered are still streamed (header-only).
        if (!writer->Write(*block)) {
            return grpc::Status(grpc::StatusCode::UNKNOWN, "stream write failed");
        }
    }
    return grpc::Status::OK;
}

}  // namespace lyghtd
