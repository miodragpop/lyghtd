#include "ingestor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <iostream>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include "block_parser.h"

namespace lyghtd {

namespace {

// Wall-clock "YYYY-MM-DD HH:MM:SS" prefix for log lines, so ingestion rate is
// readable straight off the log/screenshot.
std::string TimeStamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

// 256-entry lookup table: hex char -> nibble (0..15), 0xff for any non-hex
// byte. Decoding via one table load per char (no per-nibble branches, no
// push_back) is ~5-10x faster than the comparison ladder, and hex decode was
// the single biggest CPU cost in the ingestor (perf: ~42% self on big blocks).
constexpr std::array<uint8_t, 256> MakeHexLut() {
    std::array<uint8_t, 256> t{};
    for (auto& v : t) v = 0xff;
    for (int i = 0; i < 10; ++i) t[static_cast<size_t>('0' + i)] =
        static_cast<uint8_t>(i);
    for (int i = 0; i < 6; ++i) {
        t[static_cast<size_t>('a' + i)] = static_cast<uint8_t>(10 + i);
        t[static_cast<size_t>('A' + i)] = static_cast<uint8_t>(10 + i);
    }
    return t;
}
constexpr std::array<uint8_t, 256> kHexLut = MakeHexLut();

std::string HexDecode(std::string_view hex) {
    if (hex.size() % 2 != 0) throw std::runtime_error("odd-length block hex");
    std::string out(hex.size() / 2, '\0');
    const auto* in = reinterpret_cast<const unsigned char*>(hex.data());
    auto* dst = reinterpret_cast<unsigned char*>(out.data());
    for (size_t i = 0, j = 0; i < hex.size(); i += 2, ++j) {
        uint8_t hi = kHexLut[in[i]], lo = kHexLut[in[i + 1]];
        // Both valid nibbles are 0x00..0x0f; 0xff (invalid) sets the high bit.
        if ((hi | lo) & 0x80) throw std::runtime_error("bad block hex digit");
        dst[j] = static_cast<unsigned char>((hi << 4) | lo);
    }
    return out;
}

// True if -8 "out of range" (daemon doesn't have this height yet).
bool IsHeightTooHigh(const std::exception& e) {
    std::string msg = e.what();
    return msg.find("-8") != std::string::npos ||
           msg.find("out of range") != std::string::npos;
}

}  // namespace

TreeState SeedTreeState(BlockCache& cache) {
    TreeState ts;
    auto tip = cache.LatestHeight();
    if (tip) {
        auto block = cache.Get(*tip);
        if (block) {
            ts.sapling = block->chainmetadata().saplingcommitmenttreesize();
            ts.orchard = block->chainmetadata().orchardcommitmenttreesize();
        }
    }
    return ts;
}

std::optional<rpc::CompactBlock> GetBlockFromRPC(RpcClient& rpc, uint64_t height,
                                                 TreeState& ts,
                                                 uint64_t nu5_height) {
    try {
        if (height < nu5_height) {
            // Raw only — parser computes hash/txids itself (no V5 here).
            std::vector<std::string> raw = rpc.GetBlocksRawBatched(height, 1);
            return ParseBlockToCompact(raw[0], height, ts.sapling, ts.orchard);
        }
        // V5 possible: verbose supplies the txids; raw supplies the bytes.
        RpcClient::BlockFetch f = rpc.GetBlockBatched(height);
        std::string raw = HexDecode(f.raw_hex);
        return ParseBlockToCompact(raw, height, ts.sapling, ts.orchard,
                                   &f.verbose.tx);
    } catch (const std::exception& e) {
        if (IsHeightTooHigh(e)) return std::nullopt;
        throw;
    }
}

uint64_t IngestUpTo(BlockCache& cache, RpcClient& rpc, uint64_t target_height,
                    uint64_t nu5_height) {
    TreeState ts = SeedTreeState(cache);
    uint64_t added = 0;
    while (cache.NextHeight() <= target_height) {
        uint64_t height = cache.NextHeight();
        auto block = GetBlockFromRPC(rpc, height, ts, nu5_height);
        if (!block) break;  // daemon doesn't have this height yet
        if (cache.HashMatch(block->prevhash())) {
            cache.Add(height, *block);
            ++added;
        } else if (height == 0) {
            throw std::runtime_error("genesis prevHash mismatch");
        } else {
            // Reorg: drop the tip and re-seed the tree state from the new tip.
            cache.Reorg(height - 1);
            ts = SeedTreeState(cache);
        }
    }
    return added;
}

namespace {

struct FetchedBatch {
    uint64_t start = 0;
    std::vector<std::string> raw;                 // binary block bytes
    std::vector<std::vector<std::string>> txids;  // verbose txids/block; empty if raw-only
};

// Producer-consumer pipeline for the deep (reorg-safe) range [NextHeight(), end]:
// a producer thread fetches batches of `batch_size` over the RPC connection while
// THIS (calling) thread parses+writes them — overlapping ycashd's fetch/compute
// with our parse/cache-write so ycashd's core stays busy. The single writer still
// Adds strictly in height order (so the cache stays byte-for-byte).
//
// When `use_compact`, blocks are fetched as ready-made CompactBlock protos via
// getcompactblockrange (ycashd does all the work) and stored verbatim with
// AddRaw — no parser, no TreeState, no NU5 split. Otherwise the standard path:
// below `nu5_height` raw-only (1 call/block, txids via SHA256d, advancing `ts`),
// else verbose+raw. Logs rate; returns blocks added; throws on fetch/parse error
// (caller retries). Stops early on `stop`. The producer is always joined.
uint64_t IngestDeepPipeline(BlockCache& cache, RpcClient& rpc, uint64_t end,
                            uint64_t batch_size, std::atomic<bool>& stop,
                            bool use_compact, TreeState& ts, uint64_t nu5_height,
                            std::chrono::steady_clock::time_point& last_log,
                            uint64_t& last_log_h) {
    using namespace std::chrono_literals;
    constexpr size_t kQueueCap = 2;  // double-buffer: enough overlap, bounds memory

    std::queue<FetchedBatch> q;
    std::mutex m;
    std::condition_variable not_full, not_empty;
    bool producer_done = false;  // guarded by m
    std::string err;             // guarded by m (first fetch error, or abort)

    const uint64_t start = cache.NextHeight();

    std::thread producer([&] {
        uint64_t h = start;
        while (h <= end && !stop.load()) {
            {
                std::lock_guard<std::mutex> lk(m);
                if (!err.empty()) break;
            }
            FetchedBatch fb;
            fb.start = h;
            uint64_t count = std::min(batch_size, end - h + 1);
            if (use_compact && count > 10000) count = 10000;  // RPC range limit
            try {
                if (use_compact) {
                    // ycashd builds the CompactBlocks; stored verbatim, no parse.
                    fb.raw = rpc.GetCompactBlockRange(h, count);
                } else if (h + count - 1 < nu5_height) {
                    // Raw only (1 call/block); hex decoded to binary in-parse.
                    fb.raw = rpc.GetBlocksRawBatched(h, count);
                } else {
                    // V5 possible: verbose supplies txids; raw the bytes.
                    auto bfs = rpc.GetBlocksBatched(h, count);
                    fb.raw.resize(count);
                    fb.txids.resize(count);
                    for (uint64_t i = 0; i < count; ++i) {
                        fb.raw[i] = HexDecode(bfs[i].raw_hex);
                        fb.txids[i] = std::move(bfs[i].verbose.tx);
                    }
                }
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lk(m);
                if (err.empty()) err = e.what();
                not_empty.notify_all();
                break;
            }
            {
                std::unique_lock<std::mutex> lk(m);
                not_full.wait(lk, [&] {
                    return q.size() < kQueueCap || stop.load() || !err.empty();
                });
                if (stop.load() || !err.empty()) break;
                q.push(std::move(fb));
                not_empty.notify_one();
            }
            h += count;
        }
        std::lock_guard<std::mutex> lk(m);
        producer_done = true;
        not_empty.notify_all();
    });

    uint64_t added = 0;
    std::string fail;            // producer error surfaced to the consumer
    std::exception_ptr eptr;     // consumer (parse) exception, rethrown after join
    try {
        for (;;) {
            FetchedBatch fb;
            {
                std::unique_lock<std::mutex> lk(m);
                not_empty.wait(lk, [&] {
                    return !q.empty() || producer_done || !err.empty() ||
                           stop.load();
                });
                if (!err.empty()) { fail = err; break; }
                if (stop.load() && q.empty()) break;
                if (q.empty()) {
                    if (producer_done) break;
                    continue;
                }
                fb = std::move(q.front());
                q.pop();
                not_full.notify_one();
            }
            for (size_t i = 0; i < fb.raw.size(); ++i) {
                const uint64_t hh = fb.start + i;
                if (use_compact) {
                    // Ready-made CompactBlock proto; AddRaw checks continuity +
                    // height and stores the bytes verbatim.
                    cache.AddRaw(hh, fb.raw[i]);
                    ++added;
                    continue;
                }
                const std::vector<std::string>* txids =
                    fb.txids.empty() ? nullptr : &fb.txids[i];
                rpc::CompactBlock cb = ParseBlockToCompact(
                    fb.raw[i], hh, ts.sapling, ts.orchard, txids);
                if (!cache.HashMatch(cb.prevhash())) {
                    throw std::runtime_error(
                        "batch ingest: prevHash break at height " +
                        std::to_string(hh));
                }
                cache.Add(hh, cb);
                ++added;
            }
            auto now = std::chrono::steady_clock::now();
            if (now - last_log >= 4s) {
                double secs =
                    std::chrono::duration<double>(now - last_log).count();
                uint64_t cur = cache.NextHeight() - 1;
                std::cout << "[" << TimeStamp() << "] ingestor: height " << cur
                          << "  ("
                          << static_cast<uint64_t>((cur - last_log_h) / secs)
                          << " blk/s, batch " << batch_size << ")" << std::endl;
                last_log = now;
                last_log_h = cur;
            }
        }
    } catch (...) {
        eptr = std::current_exception();
    }

    // Ensure the producer can exit (it may be blocked on not_full), then join.
    {
        std::lock_guard<std::mutex> lk(m);
        if (err.empty()) err = "ingest aborted";
        not_full.notify_all();
        not_empty.notify_all();
    }
    producer.join();

    if (eptr) std::rethrow_exception(eptr);
    if (!fail.empty()) throw std::runtime_error(fail);
    return added;
}

}  // namespace

void RunIngestor(BlockCache& cache, RpcClient& rpc, std::atomic<bool>& stop,
                 uint64_t batch_size, uint64_t nu5_height, bool use_compact) {
    using namespace std::chrono_literals;
    // Blocks deeper than this below the tip are final (Zcash/Ycash never reorg
    // more than 100), so they can be batch-fetched without reorg handling.
    constexpr uint64_t kReorgDepth = 100;
    if (batch_size < 1) batch_size = 1;

    // Running note-commitment tree sizes, seeded from the cache tip.
    TreeState ts = SeedTreeState(cache);
    auto last_log = std::chrono::steady_clock::now();
    uint64_t last_log_h = cache.NextHeight();

    while (!stop.load()) {
        uint64_t tip = 0;
        try {
            tip = rpc.GetBlockChainInfo().blocks;
        } catch (const std::exception& e) {
            std::cerr << "[" << TimeStamp()
                      << "] ingestor: getblockchaininfo failed: " << e.what()
                      << "\n";
            std::this_thread::sleep_for(8s);
            continue;
        }
        uint64_t height = cache.NextHeight();
        if (height > tip) {  // synced
            cache.Sync();
            std::this_thread::sleep_for(2s);
            continue;
        }

        // Deep (reorg-safe) range: producer-consumer batched prefetch up to
        // tip-100 (producer fetches while this thread parses+writes).
        if (batch_size > 1 && tip >= kReorgDepth && height + kReorgDepth <= tip) {
            try {
                IngestDeepPipeline(cache, rpc, tip - kReorgDepth, batch_size,
                                   stop, use_compact, ts, nu5_height, last_log,
                                   last_log_h);
            } catch (const std::exception& e) {
                std::cerr << "[" << TimeStamp()
                          << "] ingestor: batch prefetch failed, will retry: "
                          << e.what() << "\n";
                std::this_thread::sleep_for(8s);
                ts = SeedTreeState(cache);  // resync after a partial/aborted batch
            }
            continue;
        }

        // Top 100 (reorg-prone): single-block fetch with reorg handling, via the
        // standard parsed path (the compact deep path doesn't track tree sizes,
        // so reseed ts from the cache tip before parsing this block).
        if (use_compact) ts = SeedTreeState(cache);
        std::optional<rpc::CompactBlock> block;
        try {
            block = GetBlockFromRPC(rpc, height, ts, nu5_height);
        } catch (const std::exception& e) {
            std::cerr << "[" << TimeStamp() << "] ingestor: getblock " << height
                      << " failed, will retry: " << e.what() << "\n";
            std::this_thread::sleep_for(8s);
            ts = SeedTreeState(cache);
            continue;
        }
        if (!block) {  // tip not available yet
            cache.Sync();
            std::this_thread::sleep_for(2s);
            continue;
        }
        if (cache.HashMatch(block->prevhash())) {
            cache.Add(height, *block);
            std::cout << "[" << TimeStamp() << "] ingestor: height " << height
                      << " (tip)" << std::endl;
        } else if (height == 0) {
            throw std::runtime_error("genesis prevHash mismatch");
        } else {
            std::cout << "[" << TimeStamp()
                      << "] ingestor: REORG, dropping block " << (height - 1)
                      << "\n";
            cache.Reorg(height - 1);
            ts = SeedTreeState(cache);
        }
    }
    cache.Sync();
}

}  // namespace lyghtd
