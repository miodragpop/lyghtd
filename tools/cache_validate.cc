// cache_validate — prove the cache WRITER produces byte-for-byte identical
// blocks/lengths files to the Go-written cache. Two modes:
//
//   --mode reserialize  (fast, every block type): read CompactBlocks 0..N from
//       the Go cache via the M1 reader and re-write them through the writer into
//       a fresh dir. Validates checksum/length/offset layout + proto-serialize
//       determinism across the whole range, at disk speed (no RPC).
//
//   --mode ingest  (full pipeline, a prefix): run the real ingestor (RPC →
//       parse → Add) from height 0 to N into a fresh dir. End-to-end drop-in
//       proof. RPC-bound, so use a modest N.
//
// Either way, the fresh files (covering 0..N) are diffed byte-for-byte against
// the matching prefix of the Go cache files.
//
// usage: cache_validate --mode {reserialize|ingest} --target N
//        [--data-dir D] [--chain C] [--conf F] [--out TMPDIR]

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "block_cache.h"
#include "block_parser.h"
#include "ingestor.h"
#include "rpc_client.h"

namespace {

// Read exactly n bytes from the start of `path` (looped, since read() caps at
// ~2 GB/call and the Go blocks file is multi-GB). Returns "" if the file is
// shorter than n.
std::string ReadPrefix(const std::string& path, size_t n) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { std::cerr << "open " << path << " failed\n"; std::exit(2); }
    std::string out;
    out.resize(n);
    size_t off = 0;
    while (off < n) {
        ssize_t got = ::pread(fd, out.data() + off, n - off,
                              static_cast<off_t>(off));
        if (got < 0) { ::close(fd); std::cerr << "read " << path << "\n"; std::exit(2); }
        if (got == 0) { ::close(fd); return ""; }  // shorter than n
        off += static_cast<size_t>(got);
    }
    ::close(fd);
    return out;
}

size_t FileSize(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) { std::cerr << "open " << path << " failed\n"; std::exit(2); }
    off_t sz = ::lseek(fd, 0, SEEK_END);
    ::close(fd);
    return static_cast<size_t>(sz);
}

// First differing byte offset between two equal-length buffers, or -1 if equal.
long long FirstDiff(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return 0;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) return static_cast<long long>(i);
    return -1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string home = std::getenv("HOME") ? std::getenv("HOME") : ".";
    std::string data_dir = home + "/.lightwalletd";
    std::string chain = "main";
    std::string conf = home + "/.ycash/ycash.conf";
    std::string out = "/tmp/lyghtd_cache_validate";
    std::string mode = "reserialize";
    uint64_t target = 10000;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (a == "--data-dir") data_dir = next();
        else if (a == "--chain") chain = next();
        else if (a == "--conf") conf = next();
        else if (a == "--out") out = next();
        else if (a == "--mode") mode = next();
        else if (a == "--target") target = std::stoull(next());
    }

    // Fresh output dir.
    std::string rm = "rm -rf '" + out + "'";
    if (std::system(rm.c_str()) != 0) { /* ignore */ }

    auto ref = lyghtd::BlockCache::Open(data_dir, chain, /*writable=*/false);
    uint64_t tip = ref->LatestHeight().value_or(0);
    if (target > tip) target = tip;

    auto fresh = lyghtd::BlockCache::Open(out, chain, /*writable=*/true);

    std::cout << "mode=" << mode << " target=0.." << target << "\n";

    if (mode == "reserialize") {
        for (uint64_t h = 0; h <= target; ++h) {
            auto cb = ref->Get(h);
            if (!cb) { std::cerr << "ref missing height " << h << "\n"; return 2; }
            fresh->Add(h, *cb);
            if (h % 200000 == 0 && h > 0)
                std::cout << "  reserialized " << h << "\n";
        }
    } else if (mode == "ingest") {
        auto rpc = lyghtd::RpcClient::FromConf(conf);
        uint64_t nu5_height = rpc.GetBlockChainInfo().nu5_activation_height;
        lyghtd::SetNU5ActivationHeight(nu5_height);
        uint64_t added = lyghtd::IngestUpTo(*fresh, rpc, target, nu5_height);
        std::cout << "  ingested " << added << " blocks from ycashd\n";
        if (fresh->NextHeight() != target + 1) {
            std::cerr << "ingest stopped early at height " << fresh->NextHeight()
                      << "\n";
            return 2;
        }
    } else {
        std::cerr << "unknown mode\n";
        return 2;
    }
    fresh->Sync();

    // Byte-for-byte diff of the fresh files against the Go prefix.
    const std::string fb = out + "/db/" + chain + "/blocks";
    const std::string fl = out + "/db/" + chain + "/lengths";
    const std::string gb = data_dir + "/db/" + chain + "/blocks";
    const std::string gl = data_dir + "/db/" + chain + "/lengths";

    const size_t fl_sz = FileSize(fl), fb_sz = FileSize(fb);
    std::string fresh_lengths = ReadPrefix(fl, fl_sz);
    std::string ref_lengths = ReadPrefix(gl, fl_sz);
    std::string fresh_blocks = ReadPrefix(fb, fb_sz);
    std::string ref_blocks = ReadPrefix(gb, fb_sz);

    long long ld = ref_lengths.empty() ? 0 : FirstDiff(fresh_lengths, ref_lengths);
    long long bd = ref_blocks.empty() ? 0 : FirstDiff(fresh_blocks, ref_blocks);

    std::cout << "\n=== cache writer validation ===\n"
              << "  fresh lengths : " << fresh_lengths.size() << " B\n"
              << "  fresh blocks  : " << fresh_blocks.size() << " B\n";
    bool ok = true;
    if (ld < 0) {
        std::cout << "  lengths: MATCH (prefix byte-for-byte)\n";
    } else {
        std::cout << "  lengths: MISMATCH at byte " << ld << "\n";
        ok = false;
    }
    if (bd < 0) {
        std::cout << "  blocks : MATCH (prefix byte-for-byte)\n";
    } else {
        std::cout << "  blocks : MISMATCH at byte " << bd << "\n";
        ok = false;
    }
    std::cout << (ok ? "DROP-IN BYTE-FOR-BYTE MATCH\n" : "FAILED\n");
    return ok ? 0 : 1;
}
