#pragma once

// CompactTxStreamerImpl — milestone-1 service. Serves the four truly
// cache-only RPCs (GetLightdInfo, GetLatestBlock, GetBlock, GetBlockRange)
// entirely from the on-disk cache, with no ycashd connection. Daemon-backed
// RPCs (GetTransaction, SendTransaction, GetTreeState, etc.) land in M2.

#include "block_cache.h"
#include "service.grpc.pb.h"

namespace lyghtd {

class CompactTxStreamerImpl final : public rpc::CompactTxStreamer::Service {
public:
    explicit CompactTxStreamerImpl(BlockCache* cache) : cache_(cache) {}

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

private:
    BlockCache* cache_;  // not owned
};

}  // namespace lyghtd
