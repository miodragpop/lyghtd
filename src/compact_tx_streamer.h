#pragma once

// CompactTxStreamerImpl — the gRPC service. The four truly cache-only RPCs
// (GetLightdInfo, GetLatestBlock, GetBlock, GetBlockRange) are served entirely
// from the on-disk cache. The transparent-address suite (GetTransaction,
// GetTaddress*, GetAddressUtxos*) proxies ycashd via an optional RpcClient; if
// none is wired (no reachable daemon) those return UNAVAILABLE.

#include "block_cache.h"
#include "rpc_client.h"
#include "service.grpc.pb.h"

namespace lyghtd {

class CompactTxStreamerImpl final : public rpc::CompactTxStreamer::Service {
public:
    // `rpc` may be null (cache-only serving); the daemon-backed RPCs then fail
    // with UNAVAILABLE. Neither pointer is owned.
    explicit CompactTxStreamerImpl(BlockCache* cache, RpcClient* rpc = nullptr)
        : cache_(cache), rpc_(rpc) {}

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

private:
    BlockCache* cache_;  // not owned
    RpcClient* rpc_;     // not owned; may be null (cache-only serving)
};

}  // namespace lyghtd
