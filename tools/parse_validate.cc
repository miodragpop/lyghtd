// parse_validate — validate the block parser in isolation against the Go cache.
// For each sampled height, fetch the full block from ycashd, parse it to a
// CompactBlock with the C++ parser, and compare byte-for-byte against the
// CompactBlock the Go server already stored in the cache (read via the M1
// reader). No disk writes — this proves parser correctness alone.
//
// usage: parse_validate [--data-dir D] [--chain C] [--conf F]
//                       [--ranges "lo-hi,lo-hi,..."] [--step N]
//
// Default samples a spread across the chain plus dense shielded ranges.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "block_cache.h"
#include "block_parser.h"
#include "rpc_client.h"

namespace {

std::string Ser(const google::protobuf::Message& m) {
    std::string s;
    m.SerializeToString(&s);
    return s;
}

std::string Hex(const std::string& s) {
    static const char* d = "0123456789abcdef";
    std::string o;
    for (unsigned char c : s) {
        o.push_back(d[c >> 4]);
        o.push_back(d[c & 0xf]);
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    std::string home = std::getenv("HOME") ? std::getenv("HOME") : ".";
    std::string data_dir = home + "/.lightwalletd";
    std::string chain = "main";
    std::string conf = home + "/.ycash/ycash.conf";
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    uint64_t step = 1;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--data-dir") data_dir = next();
        else if (a == "--chain") chain = next();
        else if (a == "--conf") conf = next();
        else if (a == "--step") step = std::stoull(next());
        else if (a == "--ranges") {
            std::string r = next();
            size_t p = 0;
            while (p < r.size()) {
                size_t comma = r.find(',', p);
                std::string tok = r.substr(p, comma - p);
                size_t dash = tok.find('-');
                uint64_t lo = std::stoull(tok.substr(0, dash));
                uint64_t hi = std::stoull(tok.substr(dash + 1));
                ranges.emplace_back(lo, hi);
                if (comma == std::string::npos) break;
                p = comma + 1;
            }
        }
    }

    std::unique_ptr<lyghtd::BlockCache> cache;
    try {
        cache = lyghtd::BlockCache::Open(data_dir, chain);
    } catch (const std::exception& e) {
        std::cerr << "cache open failed: " << e.what() << "\n";
        return 2;
    }
    auto rpc = lyghtd::RpcClient::FromConf(conf);
    lyghtd::SetNU5ActivationHeight(rpc.GetBlockChainInfo().nu5_activation_height);
    uint64_t tip = cache->LatestHeight().value_or(0);

    if (ranges.empty()) {
        // Spread of the early Sprout/transparent history (sparse), the
        // Sapling-activation boundary, dense shielded ranges, and the tip.
        ranges = {
            {1, 5000},            // earliest blocks
            {419190, 419260},     // around Sapling activation (419200)
            {570000, 570050},     // around the Ycash fork height
            {571000, 571200},     // a dense shielded range
            {1000000, 1000020}, {2000000, 2000020}, {2900000, 2900020},
        };
        if (tip > 30) ranges.emplace_back(tip - 20, tip);
    }

    uint64_t checked = 0, mismatches = 0;
    for (auto [lo, hi] : ranges) {
        if (hi > tip) hi = tip;
        for (uint64_t h = lo; h <= hi; h += step) {
            // Reference: the CompactBlock Go already stored.
            std::optional<lyghtd::rpc::CompactBlock> ref;
            try {
                ref = cache->Get(h);
            } catch (const std::exception& e) {
                std::cerr << "  cache.Get(" << h << ") failed: " << e.what()
                          << "\n";
                ++mismatches;
                continue;
            }
            if (!ref) continue;  // beyond cache tip

            // Build the same CompactBlock from ycashd via the C++ parser, which
            // computes hash/txids (SHA256d) and tree sizes itself. Seed the tree
            // counters from the previous cached block.
            lyghtd::rpc::CompactBlock got;
            try {
                uint32_t sapling = 0, orchard = 0;
                if (h > 0) {
                    auto prev = cache->Get(h - 1);
                    if (prev) {
                        sapling = prev->chainmetadata().saplingcommitmenttreesize();
                        orchard = prev->chainmetadata().orchardcommitmenttreesize();
                    }
                }
                std::vector<std::string> raws = rpc.GetBlocksRawBatched(h, 1);
                got = lyghtd::ParseBlockToCompact(raws[0], h, sapling, orchard);
            } catch (const std::exception& e) {
                std::cerr << "  height " << h << " parse failed: " << e.what()
                          << "\n";
                ++mismatches;
                continue;
            }

            ++checked;
            if (Ser(got) != Ser(*ref)) {
                ++mismatches;
                std::cerr << "  MISMATCH at height " << h << " (parsed "
                          << Ser(got).size() << "B vs cache " << Ser(*ref).size()
                          << "B)\n";
                // Point at the first differing field for debugging.
                if (got.hash() != ref->hash())
                    std::cerr << "    hash got=" << Hex(got.hash())
                              << " ref=" << Hex(ref->hash()) << "\n";
                if (got.prevhash() != ref->prevhash())
                    std::cerr << "    prevhash differs\n";
                if (got.vtx_size() != ref->vtx_size())
                    std::cerr << "    vtx count got=" << got.vtx_size()
                              << " ref=" << ref->vtx_size() << "\n";
            }
        }
        std::cout << "  range " << lo << ".." << hi << " done (checked "
                  << checked << ", mismatches " << mismatches << ")\n";
    }

    std::cout << "\n=== parser validation ===\n  blocks checked: " << checked
              << "\n  mismatches    : " << mismatches << "\n"
              << (mismatches == 0 ? "ALL MATCH\n" : "FAILED\n");
    return mismatches == 0 ? 0 : 1;
}
