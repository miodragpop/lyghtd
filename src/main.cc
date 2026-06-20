// lyghtd — C++ drop-in port of lightwalletd (Ycash).
//
// Serves the cache-only RPCs (GetLightdInfo, GetLatestBlock, GetBlock,
// GetBlockRange) over gRPC from the on-disk cache. With --ingest it also runs
// the standard-RPC ingestor: fetching blocks from ycashd, parsing them into
// CompactBlocks, and appending to the (writable) cache — a full drop-in for the
// Go lightwalletd. Without --ingest it is a read-only frontend.

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <pthread.h>
#include <signal.h>

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>

#include "block_cache.h"
#include "block_parser.h"
#include "compact_tx_streamer.h"
#include "ingestor.h"
#include "rpc_client.h"
#include "tls_creds.h"
#include "Version.h"

namespace {

// Calling grpc::Server::Shutdown() (which takes abseil mutexes) from a signal
// handler is async-signal-unsafe and aborts with "illegal recursion into Mutex
// code". Instead we BLOCK SIGINT/SIGTERM in all threads and dedicate one thread
// to sigwait() for them, then perform the shutdown from ordinary (non-signal)
// context — the standard safe pattern for a gRPC server.

std::string DefaultDataDir() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.lightwalletd";
}

std::string DefaultConf() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.ycash/ycash.conf";
}

void Usage(const char* argv0) {
    std::cerr
        << "usage: " << argv0 << " [options]\n"
        << "  --bind <addr>       gRPC listen address (default 127.0.0.1:19067)\n"
        << "  --data-dir <path>   lightwalletd data dir (default $HOME/.lightwalletd)\n"
        << "  --chain <name>      chain subdir under db/ (default main)\n"
        << "  --ingest            also ingest from ycashd into the cache (drop-in mode)\n"
        << "  --conf <path>       ycashd conf for --ingest (default $HOME/.ycash/ycash.conf)\n"
        << "  --ingest-batch <n>  blocks per batched RPC request for historical sync\n"
        << "                      (default: 2000 with getcompactblockrange, else 100)\n"
        << "  --tls-cert <path>   TLS certificate PEM (default ./cert.pem)\n"
        << "  --tls-key <path>    TLS key PEM (default ./cert.key)\n"
        << "  --gen-cert-very-insecure  serve TLS with an in-memory self-signed cert (dev)\n"
        << "  --no-tls-very-insecure    serve plaintext, no TLS (dev)\n"
        << "TLS is required by default: --tls-cert/--tls-key must exist unless a\n"
        << "-very-insecure flag is given.\n";
}

}  // namespace

int main(int argc, char** argv) {
    // Block SIGINT/SIGTERM in the main thread FIRST, before anything (gRPC,
    // curl) can spawn threads — new threads inherit this mask, so the signal
    // stays pending until our dedicated sigwait thread consumes it. Doing this
    // after gRPC setup would leave gRPC's already-spawned threads able to take
    // the signal and terminate the process by default (exit 130, no clean
    // shutdown). This must be the very first thing main does.
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigset, nullptr);

    std::string bind = "127.0.0.1:19067";
    std::string data_dir = DefaultDataDir();
    std::string chain = "main";
    std::string conf = DefaultConf();
    bool ingest = false;
    uint64_t ingest_batch = 0;  // 0 = auto: mode-aware default chosen after probe
    // TLS (mirrors lightwalletd cmd/root.go): TLS is required by default — the
    // cert+key files must exist — unless one of the two insecure escapes is set.
    std::string tls_cert = "./cert.pem";
    std::string tls_key = "./cert.key";
    bool no_tls = false;    // --no-tls-very-insecure: plaintext
    bool gen_cert = false;  // --gen-cert-very-insecure: in-memory self-signed

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "lyghtd: " << a << " requires an argument\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--bind") {
            bind = next();
        } else if (a == "--data-dir") {
            data_dir = next();
        } else if (a == "--chain") {
            chain = next();
        } else if (a == "--conf") {
            conf = next();
        } else if (a == "--ingest") {
            ingest = true;
        } else if (a == "--ingest-batch") {
            ingest_batch = std::stoull(next());
        } else if (a == "--tls-cert") {
            tls_cert = next();
        } else if (a == "--tls-key") {
            tls_key = next();
        } else if (a == "--no-tls-very-insecure") {
            no_tls = true;
        } else if (a == "--gen-cert-very-insecure") {
            gen_cert = true;
        } else if (a == "-h" || a == "--help") {
            Usage(argv[0]);
            return 0;
        } else {
            std::cerr << "lyghtd: unknown argument: " << a << "\n";
            Usage(argv[0]);
            return 2;
        }
    }

    std::unique_ptr<lyghtd::BlockCache> cache;
    try {
        // --ingest opens the cache writable so the ingestor can append.
        cache = lyghtd::BlockCache::Open(data_dir, chain, /*writable=*/ingest);
    } catch (const std::exception& e) {
        std::cerr << "lyghtd: failed to open cache at " << data_dir << "/db/"
                  << chain << "/: " << e.what() << "\n";
        return 1;
    }
    const auto tip = cache->LatestHeight();
    std::cout << "lyghtd " << lyghtd::k_version << ": opened cache (chain="
              << chain << ", " << cache->Count() << " blocks, tip height "
              << (tip ? std::to_string(*tip) : std::string("none")) << ")\n";

    lyghtd::CompactTxStreamerImpl service(cache.get());

    grpc::EnableDefaultHealthCheckService(true);
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();

    // TLS credentials (lightwalletd-compatible). Default mode requires the
    // cert+key files to exist; the two "-very-insecure" escapes opt out.
    if (!no_tls && !gen_cert) {
        auto exists = [](const std::string& p) {
            return std::ifstream(p).good();
        };
        for (const std::string& p : {tls_cert, tls_key}) {
            if (!exists(p)) {
                std::cerr << "lyghtd: required TLS file does not exist: " << p
                          << "\n  provide --tls-cert/--tls-key, or run with"
                             " --gen-cert-very-insecure / --no-tls-very-insecure\n";
                return 1;
            }
        }
    }
    std::shared_ptr<grpc::ServerCredentials> creds;
    try {
        creds = lyghtd::BuildServerCredentials(no_tls, gen_cert, tls_cert,
                                               tls_key);
    } catch (const std::exception& e) {
        std::cerr << "lyghtd: couldn't set up TLS credentials: " << e.what()
                  << "\n";
        return 1;
    }
    std::cout << "lyghtd: "
              << (no_tls ? "INSECURE plaintext server (no TLS)"
                  : gen_cert
                      ? "TLS with in-memory self-signed cert (insecure, dev only)"
                      : "TLS (cert " + tls_cert + ", key " + tls_key + ")")
              << "\n";

    grpc::ServerBuilder builder;
    builder.AddListeningPort(bind, creds);
    builder.RegisterService(&service);
    // No artificial cap on inbound/outbound message size: full CompactBlocks
    // can be large, and GetBlockRange streams many.
    builder.SetMaxReceiveMessageSize(-1);
    builder.SetMaxSendMessageSize(-1);

    std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
    if (!server) {
        std::cerr << "lyghtd: failed to bind " << bind << "\n";
        return 1;
    }

    // Optional ingestor: a single kept-alive RPC connection fetches from ycashd
    // and appends to the (writable) cache while the server reads from it
    // concurrently (shared_mutex makes that safe).
    std::atomic<bool> stop_ingest{false};
    std::unique_ptr<lyghtd::RpcClient> rpc;
    std::thread ingest_thread;
    if (ingest) {
        try {
            rpc = std::make_unique<lyghtd::RpcClient>(
                lyghtd::RpcClient::FromConf(conf));
        } catch (const std::exception& e) {
            std::cerr << "lyghtd: --ingest but cannot reach ycashd via " << conf
                      << ": " << e.what() << "\n";
            return 1;
        }
        // NU5/Orchard activation height from the daemon (UINT64_MAX = no NU5,
        // i.e. Ycash). Below it the ingestor fetches raw-only and computes txids
        // via SHA256d; at/above it it falls back to verbose+raw for V5 txids.
        uint64_t nu5_height = UINT64_MAX;
        try {
            nu5_height = rpc->GetBlockChainInfo().nu5_activation_height;
        } catch (const std::exception& e) {
            std::cerr << "lyghtd: --ingest but cannot reach ycashd via " << conf
                      << ": " << e.what() << "\n";
            return 1;
        }
        lyghtd::SetNU5ActivationHeight(nu5_height);
        // Probe the "compactblocks" experimental feature: if present, the deep
        // range is pulled as ready-made CompactBlocks via getcompactblockrange
        // (ycashd does the work); otherwise fall back to the standard getblock
        // ingestor. Old daemons without getexperimentalfeatures probe false.
        const bool use_compact = rpc->SupportsCompactBlocks();
        // Mode-aware default batch (== getcompactblockrange count in compact
        // mode). 2000 measured fastest full-chain (1788 blk/s vs 1685 @1000,
        // 1387 @10000): big enough to amortize round-trips, small enough to keep
        // the producer/consumer pipeline finely overlapped (a 10000 range makes
        // ycashd build the whole chunk before responding while lyghtd idles).
        // An explicit --ingest-batch always wins.
        if (ingest_batch == 0) ingest_batch = use_compact ? 2000 : 100;
        std::cout << "lyghtd: ingesting from ycashd at " << rpc->host() << ":"
                  << rpc->port() << " (batch " << ingest_batch << ", NU5 "
                  << (nu5_height == UINT64_MAX ? std::string("none")
                                               : std::to_string(nu5_height))
                  << ", source "
                  << (use_compact ? "getcompactblockrange" : "getblock")
                  << ")\n";
        ingest_thread = std::thread([&]() {
            lyghtd::RunIngestor(*cache, *rpc, stop_ingest, ingest_batch,
                                nu5_height, use_compact);
        });
    }

    std::thread sig_thread([&server, &sigset]() {
        int sig = 0;
        sigwait(&sigset, &sig);  // blocks until SIGINT/SIGTERM arrives
        // Now in ordinary thread context — safe to call into gRPC/abseil.
        server->Shutdown();
    });

    std::cout << "lyghtd listening on " << bind << "\n";
    server->Wait();

    // Server stopped (shutdown requested): stop the ingestor too.
    stop_ingest.store(true);
    if (ingest_thread.joinable()) ingest_thread.join();
    sig_thread.join();
    std::cout << "lyghtd: shut down cleanly\n";
    return 0;
}
