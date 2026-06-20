// ingest_profile — break down where ingestion time goes per block, to find the
// real bottleneck (RPC round-trips vs JSON parse vs block parse). Times each
// stage over a range and reports averages. Read-only on ycashd; writes nothing.
//
// usage: ingest_profile [--conf F] [--start H] [--count N]

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "block_parser.h"
#include "rpc_client.h"

using clk = std::chrono::steady_clock;
static double ms(clk::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

int main(int argc, char** argv) {
    std::string home = std::getenv("HOME") ? std::getenv("HOME") : ".";
    std::string conf = home + "/.ycash/ycash.conf";
    uint64_t start = 20000, count = 3000;
    int threads = 0;  // >0 enables the concurrent-fetch ceiling probe
    bool compact = false;     // --compact: probe getcompactblockrange instead
    uint64_t range = 10000;   // per-call getcompactblockrange size
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--conf") conf = next();
        else if (a == "--start") start = std::stoull(next());
        else if (a == "--count") count = std::stoull(next());
        else if (a == "--threads") threads = std::stoi(next());
        else if (a == "--compact") compact = true;
        else if (a == "--range") range = std::stoull(next());
    }
    lyghtd::SetNU5ActivationHeight(UINT64_MAX);  // Ycash: no NU5

    // ---- Compact-block probe (getcompactblockrange) ----
    // Isolates the ycashd-vs-lyghtd split that batch size can't fix: ycashd
    // builds the CompactBlocks (the cost), lyghtd just stores them. With
    // --threads N, N connections issue ranges concurrently to see whether
    // ycashd's compact builder scales across cores or serializes.
    if (compact) {
        if (range < 1) range = 1;
        if (range > 10000) range = 10000;  // RPC limit
        if (threads > 0) {
            std::atomic<uint64_t> done{0};
            auto t0 = clk::now();
            std::vector<std::thread> pool;
            for (int t = 0; t < threads; ++t) {
                pool.emplace_back([&, t]() {
                    auto rpc = lyghtd::RpcClient::FromConf(conf);
                    // Each thread strides disjoint range-sized chunks.
                    for (uint64_t h = start + static_cast<uint64_t>(t) * range;
                         h < start + count; h += static_cast<uint64_t>(threads) * range) {
                        uint64_t c = std::min(range, start + count - h);
                        auto blocks = rpc.GetCompactBlockRange(h, c);
                        done.fetch_add(blocks.size(), std::memory_order_relaxed);
                    }
                });
            }
            for (auto& th : pool) th.join();
            double wall = ms(clk::now() - t0) / 1000.0;
            std::cout << "concurrent getcompactblockrange: " << threads
                      << " threads, range " << range << ", " << done.load()
                      << " blocks in " << wall << " s  = "
                      << static_cast<uint64_t>(done.load() / wall) << " blk/s\n";
            return 0;
        }

        // Single connection: time the range fetch (ycashd build + transfer) vs a
        // lyghtd-side proxy (parse each compact proto, ~what AddRaw deserializes).
        auto rpc = lyghtd::RpcClient::FromConf(conf);
        double t_fetch = 0, t_parse = 0, curl_xfer = 0;
        uint64_t bytes = 0, n = 0;
        auto t0 = clk::now();
        for (uint64_t h = start; h < start + count; h += range) {
            uint64_t c = std::min(range, start + count - h);
            auto a = clk::now();
            auto blocks = rpc.GetCompactBlockRange(h, c);
            auto b = clk::now();
            curl_xfer += rpc.last_transfer_seconds() * 1000.0;
            for (auto& pb : blocks) {
                bytes += pb.size();
                lyghtd::rpc::CompactBlock cb;
                cb.ParseFromString(pb);  // proxy for AddRaw's metadata parse
            }
            auto d = clk::now();
            t_fetch += ms(b - a);
            t_parse += ms(d - b);
            n += blocks.size();
        }
        double wall = ms(clk::now() - t0);
        std::cout << "compact profile: " << n << " blocks from " << start
                  << ", range " << range << " (avg compact block "
                  << (n ? bytes / n : 0) << " B)\n"
                  << "  total wall   : " << wall / 1000.0 << " s  ("
                  << static_cast<uint64_t>(n / (wall / 1000.0)) << " blk/s)\n"
                  << "  per-block avg (ms):\n"
                  << "    getcompactblockrange (ycashd build + transfer): "
                  << (n ? t_fetch / n : 0) << "   (curl transfer "
                  << (n ? curl_xfer / n : 0) << ")\n"
                  << "    lyghtd-side proto parse (~AddRaw)              : "
                  << (n ? t_parse / n : 0) << "\n"
                  << "  ycashd share: "
                  << static_cast<int>(100 * t_fetch / (t_fetch + t_parse + 1e-9))
                  << "%  (lyghtd share "
                  << static_cast<int>(100 * t_parse / (t_fetch + t_parse + 1e-9))
                  << "%)\n";
        return 0;
    }

    // Concurrency probe: N threads (each its own connection) fetch raw blocks by
    // height in parallel. Measures ycashd's concurrent getblock ceiling — i.e.
    // how much a prefetch worker pool could overlap the ~20ms server waits.
    if (threads > 0) {
        std::atomic<uint64_t> done{0};
        auto t0 = clk::now();
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t) {
            pool.emplace_back([&, t]() {
                auto rpc = lyghtd::RpcClient::FromConf(conf);
                for (uint64_t h = start + t; h < start + count; h += threads) {
                    rpc.RawRequest("getblock", "[\"" + std::to_string(h) + "\",0]");
                    done.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        for (auto& th : pool) th.join();
        double wall = ms(clk::now() - t0) / 1000.0;
        std::cout << "concurrent getblock raw: " << threads << " threads, "
                  << done.load() << " blocks in " << wall << " s  = "
                  << static_cast<uint64_t>(done.load() / wall) << " blk/s\n";
        return 0;
    }

    auto rpc = lyghtd::RpcClient::FromConf(conf);

    double t_verbose = 0, t_raw = 0, t_hexdecode = 0, t_parse = 0;
    double curl_verbose = 0, curl_raw = 0;  // curl's own transfer time
    auto t0 = clk::now();
    uint64_t bytes_raw = 0;
    for (uint64_t h = start; h < start + count; ++h) {
        auto a = clk::now();
        auto vb = rpc.GetBlockVerbose(h);
        curl_verbose += rpc.last_transfer_seconds() * 1000.0;
        auto b = clk::now();
        std::string rawhex = rpc.GetBlockRawHex(vb.hash);
        curl_raw += rpc.last_transfer_seconds() * 1000.0;
        auto c = clk::now();
        std::string raw;
        raw.reserve(rawhex.size() / 2);
        auto nib = [](char ch) { return ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10; };
        for (size_t i = 0; i + 1 < rawhex.size(); i += 2)
            raw.push_back(static_cast<char>((nib(rawhex[i]) << 4) | nib(rawhex[i + 1])));
        auto d = clk::now();
        uint32_t sapling = 0, orchard = 0;  // per-call (timing only, not cumulative)
        auto cb = lyghtd::ParseBlockToCompact(raw, h, sapling, orchard);
        auto e = clk::now();
        t_verbose += ms(b - a);
        t_raw += ms(c - b);
        t_hexdecode += ms(d - c);
        t_parse += ms(e - d);
        bytes_raw += raw.size();
        (void)cb;
    }
    double wall = ms(clk::now() - t0);

    std::cout << "profiled " << count << " blocks from height " << start
              << " (avg raw block " << (bytes_raw / count) << " B)\n"
              << "  total wall      : " << wall / 1000.0 << " s  ("
              << static_cast<uint64_t>(count / (wall / 1000.0)) << " blk/s)\n"
              << "  per-block avg (ms):\n"
              << "    getblock verbose total: " << t_verbose / count
              << "   (curl transfer " << curl_verbose / count
              << ", our JSON " << (t_verbose - curl_verbose) / count << ")\n"
              << "    getblock raw     total: " << t_raw / count
              << "   (curl transfer " << curl_raw / count
              << ", our JSON " << (t_raw - curl_raw) / count << ")\n"
              << "    hex decode            : " << t_hexdecode / count << "\n"
              << "    block parse           : " << t_parse / count << "\n";
    double rpc_total = t_verbose + t_raw;
    std::cout << "  RPC share: "
              << static_cast<int>(100 * rpc_total / (t_verbose + t_raw + t_hexdecode + t_parse))
              << "%  (parse+decode share: "
              << static_cast<int>(100 * (t_hexdecode + t_parse) /
                                  (t_verbose + t_raw + t_hexdecode + t_parse))
              << "%)\n";
    return 0;
}
