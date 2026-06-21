#include "compact_tx_streamer.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <functional>
#include <string>
#include <vector>

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

// ---- Transparent-address-suite helpers ----

grpc::Status NoRpc() {
    return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                        "no ycashd connection configured");
}

// Ycash transparent address: 's' + 34 alphanumeric (P2PKH s1.., P2SH s3..).
// Upstream lightwalletd checks a 't' prefix; Ycash uses 's'.
grpc::Status CheckTaddress(const std::string& a) {
    const bool ok =
        a.size() == 35 && a[0] == 's' &&
        std::all_of(a.begin(), a.end(),
                    [](unsigned char c) { return std::isalnum(c) != 0; });
    if (!ok) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "transparent address contains invalid characters: " + a);
    }
    return grpc::Status::OK;
}

std::string HexDecode(const std::string& hex) {
    auto nib = [](char c) -> int {
        return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
    };
    std::string out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<char>((nib(hex[i]) << 4) | nib(hex[i + 1])));
    }
    return out;
}

std::string ToHex(const std::string& b) {
    static const char* k = "0123456789abcdef";
    std::string out;
    out.reserve(b.size() * 2);
    for (unsigned char c : b) {
        out.push_back(k[c >> 4]);
        out.push_back(k[c & 0xf]);
    }
    return out;
}

std::string Reversed(const std::string& s) {
    return std::string(s.rbegin(), s.rend());
}

// Map a ycashd address-RPC error string to a gRPC code, like Go lightwalletd.
grpc::Status MapAddrErr(const std::exception& e) {
    const std::string m = e.what();
    if (m.find("Invalid address") != std::string::npos) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, m);
    }
    if (m.find("No information available") != std::string::npos) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, m);
    }
    return grpc::Status(grpc::StatusCode::UNKNOWN, m);
}

// Fetch one tx by big-endian display-hex txid into `out` (getrawtransaction).
grpc::Status FetchRawTx(RpcClient* rpc, const std::string& txid_be_hex,
                        rpc::RawTransaction* out) {
    try {
        RawTx t = rpc->GetRawTransaction(txid_be_hex);
        out->set_data(t.data);
        // Reinterpret int64 height as uint64 exactly like Go (−1 -> fork
        // sentinel, 0 -> mempool).
        out->set_height(static_cast<uint64_t>(t.height));
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            std::string("getrawtransaction failed: ") + e.what());
    }
    return grpc::Status::OK;
}

grpc::Status BalanceOf(RpcClient* rpc, const std::vector<std::string>& addrs,
                       rpc::Balance* resp) {
    for (const auto& a : addrs) {
        grpc::Status st = CheckTaddress(a);
        if (!st.ok()) return st;
    }
    try {
        resp->set_valuezat(rpc->GetAddressBalance(addrs));
    } catch (const std::exception& e) {
        return MapAddrErr(e);
    }
    return grpc::Status::OK;
}

// Shared getaddressutxos path: validate, fetch, filter by startHeight/maxEntries,
// map to GetAddressUtxosReply (txid -> wire order), and hand each to `emit`.
grpc::Status ForEachUtxo(
    RpcClient* rpc, const rpc::GetAddressUtxosArg* arg,
    const std::function<grpc::Status(const rpc::GetAddressUtxosReply&)>& emit) {
    std::vector<std::string> addrs(arg->addresses().begin(),
                                   arg->addresses().end());
    for (const auto& a : addrs) {
        grpc::Status st = CheckTaddress(a);
        if (!st.ok()) return st;
    }
    std::vector<AddressUtxo> utxos;
    try {
        utxos = rpc->GetAddressUtxos(addrs);
    } catch (const std::exception& e) {
        return MapAddrErr(e);
    }
    uint32_t n = 0;
    for (const auto& u : utxos) {
        if (static_cast<uint64_t>(u.height) < arg->startheight()) continue;
        ++n;
        if (arg->maxentries() > 0 && n > arg->maxentries()) break;
        rpc::GetAddressUtxosReply r;
        r.set_address(u.address);
        r.set_txid(Reversed(HexDecode(u.txid)));  // wire (little-endian) order
        r.set_index(static_cast<int32_t>(u.output_index));
        r.set_script(HexDecode(u.script));
        r.set_valuezat(static_cast<int64_t>(u.satoshis));
        r.set_height(static_cast<uint64_t>(u.height));
        grpc::Status s = emit(r);
        if (!s.ok()) return s;
    }
    return grpc::Status::OK;
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

// ---- Transparent-address suite (daemon-backed) ----

grpc::Status CompactTxStreamerImpl::GetTransaction(grpc::ServerContext*,
                                                   const rpc::TxFilter* req,
                                                   rpc::RawTransaction* resp) {
    if (!rpc_) return NoRpc();
    if (!req->hash().empty()) {
        if (req->hash().size() != 32) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "transaction ID has invalid length");
        }
        // hash is wire (little-endian) order; getrawtransaction wants big-endian
        // display hex.
        return FetchRawTx(rpc_, ToHex(Reversed(req->hash())), resp);
    }
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "specify a txid");
}

grpc::Status CompactTxStreamerImpl::GetTaddressTransactions(
    grpc::ServerContext*, const rpc::TransparentAddressBlockFilter* req,
    grpc::ServerWriter<rpc::RawTransaction>* writer) {
    if (!rpc_) return NoRpc();
    grpc::Status st = CheckTaddress(req->address());
    if (!st.ok()) return st;
    if (!req->has_range() || !req->range().has_start()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "must specify a start block height");
    }
    const uint64_t start = req->range().start().height();
    const uint64_t end = req->range().has_end() ? req->range().end().height() : 0;

    std::vector<std::string> txids;
    try {
        txids = rpc_->GetAddressTxids({req->address()}, start, end);
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            std::string("getaddresstxids failed: ") + e.what());
    }
    for (const auto& txid_be : txids) {
        rpc::RawTransaction tx;
        grpc::Status s = FetchRawTx(rpc_, txid_be, &tx);
        if (!s.ok()) return s;
        if (!writer->Write(tx)) {
            return grpc::Status(grpc::StatusCode::UNKNOWN, "stream write failed");
        }
    }
    return grpc::Status::OK;
}

grpc::Status CompactTxStreamerImpl::GetTaddressTxids(
    grpc::ServerContext* ctx, const rpc::TransparentAddressBlockFilter* req,
    grpc::ServerWriter<rpc::RawTransaction>* writer) {
    // Deprecated alias — identical to GetTaddressTransactions (matches Go).
    return GetTaddressTransactions(ctx, req, writer);
}

grpc::Status CompactTxStreamerImpl::GetTaddressBalance(grpc::ServerContext*,
                                                       const rpc::AddressList* req,
                                                       rpc::Balance* resp) {
    if (!rpc_) return NoRpc();
    return BalanceOf(rpc_, {req->addresses().begin(), req->addresses().end()},
                     resp);
}

grpc::Status CompactTxStreamerImpl::GetTaddressBalanceStream(
    grpc::ServerContext*, grpc::ServerReader<rpc::Address>* reader,
    rpc::Balance* resp) {
    if (!rpc_) return NoRpc();
    std::vector<std::string> addrs;
    rpc::Address a;
    while (reader->Read(&a)) addrs.push_back(a.address());
    return BalanceOf(rpc_, addrs, resp);
}

grpc::Status CompactTxStreamerImpl::GetAddressUtxos(
    grpc::ServerContext*, const rpc::GetAddressUtxosArg* req,
    rpc::GetAddressUtxosReplyList* resp) {
    if (!rpc_) return NoRpc();
    return ForEachUtxo(rpc_, req, [resp](const rpc::GetAddressUtxosReply& r) {
        *resp->add_addressutxos() = r;
        return grpc::Status::OK;
    });
}

grpc::Status CompactTxStreamerImpl::GetAddressUtxosStream(
    grpc::ServerContext*, const rpc::GetAddressUtxosArg* req,
    grpc::ServerWriter<rpc::GetAddressUtxosReply>* writer) {
    if (!rpc_) return NoRpc();
    return ForEachUtxo(rpc_, req, [writer](const rpc::GetAddressUtxosReply& r) {
        if (!writer->Write(r)) {
            return grpc::Status(grpc::StatusCode::UNKNOWN, "stream write failed");
        }
        return grpc::Status::OK;
    });
}

// ---- Spend path (daemon-backed) ----

grpc::Status CompactTxStreamerImpl::SendTransaction(grpc::ServerContext*,
                                                    const rpc::RawTransaction* req,
                                                    rpc::SendResponse* resp) {
    if (!rpc_) return NoRpc();
    if (req->data().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "bad transaction data");
    }
    SendResult sr;
    try {
        sr = rpc_->SendRawTransaction(ToHex(req->data()));
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::UNKNOWN, e.what());
    }
    // On success: code 0, message = the raw quoted txid (matches Go). On an RPC
    // rejection: the daemon's code/message, still a normal (OK) response.
    if (sr.ok) {
        resp->set_errorcode(0);
        resp->set_errormessage(sr.result);
    } else {
        resp->set_errorcode(sr.error_code);
        resp->set_errormessage(sr.error_message);
    }
    return grpc::Status::OK;
}

grpc::Status CompactTxStreamerImpl::GetTreeState(grpc::ServerContext* ctx,
                                                 const rpc::BlockID* req,
                                                 rpc::TreeState* resp) {
    if (!rpc_) return NoRpc();
    if (req->height() == 0 && req->hash().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "must specify a block height or ID (hash)");
    }
    // z_gettreestate takes a height (as a decimal string) or a block hash hex.
    // BlockID.hash is big-endian — kept as-is (no reversal).
    std::string arg = req->height() > 0 ? std::to_string(req->height())
                                        : ToHex(req->hash());

    TreeStateResult ts;
    for (;;) {  // walk back via skipHash for pre-Sapling heights
        if (ctx->IsCancelled()) {
            return grpc::Status(grpc::StatusCode::CANCELLED, "client cancelled");
        }
        try {
            ts = rpc_->GetTreeState(arg);
        } catch (const std::exception& e) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                std::string("z_gettreestate failed: ") + e.what());
        }
        if (!ts.sapling_final.empty() || ts.sapling_skip_hash.empty()) break;
        arg = ts.sapling_skip_hash;
    }
    if (ts.sapling_final.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "z_gettreestate did not return treestate");
    }
    resp->set_network(kChainName);
    resp->set_height(static_cast<uint64_t>(ts.height));
    resp->set_hash(ts.hash);
    resp->set_time(ts.time);
    resp->set_saplingtree(ts.sapling_final);
    resp->set_orchardtree(ts.orchard_final);
    return grpc::Status::OK;
}

grpc::Status CompactTxStreamerImpl::GetLatestTreeState(grpc::ServerContext* ctx,
                                                       const rpc::Empty*,
                                                       rpc::TreeState* resp) {
    if (!rpc_) return NoRpc();
    uint64_t tip = 0;
    try {
        tip = rpc_->GetBlockChainInfo().blocks;
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                            std::string("getblockchaininfo failed: ") + e.what());
    }
    rpc::BlockID id;
    id.set_height(tip);
    return GetTreeState(ctx, &id, resp);
}

}  // namespace lyghtd
