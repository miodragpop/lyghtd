#pragma once

// CompactTxStreamerImpl — the gRPC service. The four truly cache-only RPCs
// (GetLightdInfo, GetLatestBlock, GetBlock, GetBlockRange) are served entirely
// from the on-disk cache. The transparent-address suite (GetTransaction,
// GetTaddress*, GetAddressUtxos*) proxies ycashd via an optional RpcClient; if
// none is wired (no reachable daemon) those return UNAVAILABLE.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "block_cache.h"
#include "rpc_client.h"
#include "service.grpc.pb.h"

namespace lyghtd {

class CompactTxStreamerImpl final : public rpc::CompactTxStreamer::Service {
public:
    // `rpc` may be null (cache-only serving); the daemon-backed RPCs then fail
    // with UNAVAILABLE. Neither pointer is owned. `ping_enable` gates the Ping
    // RPC (lightwalletd --ping-very-insecure); off by default.
    explicit CompactTxStreamerImpl(BlockCache* cache, RpcClient* rpc = nullptr,
                                   bool ping_enable = false)
        : cache_(cache), rpc_(rpc), ping_enable_(ping_enable) {}

    grpc::Status GetLightdInfo(grpc::ServerContext* ctx, const rpc::Empty* req,
                               rpc::LightdInfo* resp) override;

    grpc::Status GetLatestBlock(grpc::ServerContext* ctx,
                                const rpc::ChainSpec* req,
                                rpc::BlockID* resp) override;

    grpc::Status GetBlock(grpc::ServerContext* ctx, const rpc::BlockID* req,
                          rpc::CompactBlock* resp) override;

    grpc::Status GetBlockRange(
        grpc::ServerContext* ctx, const rpc::BlockRange* req,
        grpc::ServerWriter<rpc::CompactBlock>* writer) override;

    // ---- Transparent-address suite (daemon-backed) ----

    grpc::Status GetTransaction(grpc::ServerContext* ctx,
                                const rpc::TxFilter* req,
                                rpc::RawTransaction* resp) override;

    grpc::Status GetTaddressTxids(
        grpc::ServerContext* ctx, const rpc::TransparentAddressBlockFilter* req,
        grpc::ServerWriter<rpc::RawTransaction>* writer) override;

    grpc::Status GetTaddressTransactions(
        grpc::ServerContext* ctx, const rpc::TransparentAddressBlockFilter* req,
        grpc::ServerWriter<rpc::RawTransaction>* writer) override;

    grpc::Status GetTaddressBalance(grpc::ServerContext* ctx,
                                    const rpc::AddressList* req,
                                    rpc::Balance* resp) override;

    grpc::Status GetTaddressBalanceStream(
        grpc::ServerContext* ctx, grpc::ServerReader<rpc::Address>* reader,
        rpc::Balance* resp) override;

    grpc::Status GetAddressUtxos(grpc::ServerContext* ctx,
                                 const rpc::GetAddressUtxosArg* req,
                                 rpc::GetAddressUtxosReplyList* resp) override;

    grpc::Status GetAddressUtxosStream(
        grpc::ServerContext* ctx, const rpc::GetAddressUtxosArg* req,
        grpc::ServerWriter<rpc::GetAddressUtxosReply>* writer) override;

    // ---- Spend path (daemon-backed) ----

    grpc::Status SendTransaction(grpc::ServerContext* ctx,
                                 const rpc::RawTransaction* req,
                                 rpc::SendResponse* resp) override;

    grpc::Status GetTreeState(grpc::ServerContext* ctx, const rpc::BlockID* req,
                              rpc::TreeState* resp) override;

    grpc::Status GetLatestTreeState(grpc::ServerContext* ctx,
                                    const rpc::Empty* req,
                                    rpc::TreeState* resp) override;

    // ---- Mempool (daemon-backed) ----

    grpc::Status GetMempoolTx(
        grpc::ServerContext* ctx, const rpc::GetMempoolTxRequest* req,
        grpc::ServerWriter<rpc::CompactTx>* writer) override;

    grpc::Status GetMempoolStream(
        grpc::ServerContext* ctx, const rpc::Empty* req,
        grpc::ServerWriter<rpc::RawTransaction>* writer) override;

    // ---- Nullifier-only blocks (cache) / subtree roots / ping ----

    grpc::Status GetBlockNullifiers(grpc::ServerContext* ctx,
                                    const rpc::BlockID* req,
                                    rpc::CompactBlock* resp) override;

    grpc::Status GetBlockRangeNullifiers(
        grpc::ServerContext* ctx, const rpc::BlockRange* req,
        grpc::ServerWriter<rpc::CompactBlock>* writer) override;

    grpc::Status GetSubtreeRoots(
        grpc::ServerContext* ctx, const rpc::GetSubtreeRootsArg* req,
        grpc::ServerWriter<rpc::SubtreeRoot>* writer) override;

    grpc::Status Ping(grpc::ServerContext* ctx, const rpc::Duration* req,
                      rpc::PingResponse* resp) override;

private:
    // Fetch any newly-seen mempool txs into mempool_list_. Caller holds
    // mempool_mu_. Mirrors Go's refreshMempoolTxns. Throws on RPC error.
    void RefreshMempoolLocked();

    BlockCache* cache_;  // not owned
    RpcClient* rpc_;     // not owned; may be null (cache-only serving)
    bool ping_enable_;   // gates the Ping RPC
    std::atomic<int64_t> ping_concurrent_{0};  // in-flight Ping count

    // Shared GetMempoolStream state (mirrors Go lightwalletd's package globals),
    // so the first stream after a new block resets and returns, and concurrent
    // streams share one mempool view. All guarded by mempool_mu_.
    std::mutex mempool_mu_;
    std::set<std::string> mempool_seen_;            // txids seen this interval
    std::vector<rpc::RawTransaction> mempool_list_;  // txs to send, in order
    std::chrono::steady_clock::time_point mempool_last_fetch_{};  // epoch = never
    std::string mempool_last_hash_;                  // best block hash last seen
};

}  // namespace lyghtd
