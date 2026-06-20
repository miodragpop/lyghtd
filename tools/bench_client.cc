// bench_client — serving throughput regression check. Streams
// GetBlockRange(1..tip) from a running lyghtd (or any CompactTxStreamer) and
// reports wall time, blocks/s, bytes, and tx count. Plaintext, single stream,
// default shielded-only filter — matching the Go lightwalletd baseline.
//
// usage: bench_client [addr]   (default 127.0.0.1:19067)

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include "service.grpc.pb.h"

namespace rpc = cash::z::wallet::sdk::rpc;
using clk = std::chrono::steady_clock;

int main(int argc, char** argv) {
    const std::string addr = argc > 1 ? argv[1] : "127.0.0.1:19067";

    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(-1);
    auto ch = grpc::CreateCustomChannel(
        addr, grpc::InsecureChannelCredentials(), args);
    auto stub = rpc::CompactTxStreamer::NewStub(ch);

    uint64_t tip = 0;
    {
        rpc::ChainSpec req;
        rpc::BlockID resp;
        grpc::ClientContext c;
        auto s = stub->GetLatestBlock(&c, req, &resp);
        if (!s.ok()) {
            std::cerr << "GetLatestBlock failed: " << s.error_message() << "\n";
            return 1;
        }
        tip = resp.height();
    }
    std::cout << "Streaming GetBlockRange(1.." << tip
              << ") shielded-only, plaintext, single stream...\n";

    rpc::BlockRange req;
    req.mutable_start()->set_height(1);
    req.mutable_end()->set_height(tip);

    grpc::ClientContext ctx;
    auto reader = stub->GetBlockRange(&ctx, req);

    uint64_t blocks = 0, txs = 0, compact_bytes = 0;
    auto t0 = clk::now();
    auto last = t0;
    uint64_t last_blocks = 0;

    rpc::CompactBlock blk;
    while (reader->Read(&blk)) {
        ++blocks;
        compact_bytes += blk.ByteSizeLong();
        txs += blk.vtx_size();
        if (blocks - last_blocks >= 250000) {
            auto now = clk::now();
            double dt = std::chrono::duration<double>(now - last).count();
            std::cout << "  h=" << blk.height() << "  " << blocks << " blocks  ("
                      << static_cast<uint64_t>((blocks - last_blocks) / dt)
                      << " blk/s inst)\n";
            last = now;
            last_blocks = blocks;
        }
    }
    auto status = reader->Finish();
    auto t1 = clk::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    if (!status.ok()) {
        std::cerr << "stream error after " << blocks
                  << " blocks: " << status.error_message() << "\n";
        return 1;
    }

    std::cout << "\n=== serving benchmark ===\n"
              << "  blocks streamed : " << blocks << "\n"
              << "  compact txs     : " << txs << "\n"
              << "  compact bytes   : " << compact_bytes << " ("
              << (compact_bytes / 1e9) << " GB)\n"
              << "  wall time       : " << secs << " s ("
              << static_cast<int>(secs) / 60 << "m" << static_cast<int>(secs) % 60
              << "s)\n"
              << "  overall rate    : " << static_cast<uint64_t>(blocks / secs)
              << " blk/s\n"
              << "  throughput      : " << (compact_bytes / 1e6 / secs)
              << " MB/s\n";
    return 0;
}
