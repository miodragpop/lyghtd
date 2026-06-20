#include "block_cache.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace lyghtd {

namespace {

// pwrite the full buffer or fail; loops over short writes.
bool PwriteFull(int fd, const void* buf, size_t count, off_t offset) {
    const auto* p = static_cast<const char*>(buf);
    while (count > 0) {
        ssize_t n = ::pwrite(fd, p, count, offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        offset += n;
        count -= static_cast<size_t>(n);
    }
    return true;
}

// Create <data_dir>/db/<chain>/ (and parents) if missing.
void MakeDirs(const std::string& dir) {
    std::string acc;
    for (size_t i = 0; i < dir.size(); ++i) {
        acc.push_back(dir[i]);
        if (dir[i] == '/' && acc.size() > 1) {
            ::mkdir(acc.c_str(), 0755);  // ignore EEXIST
        }
    }
    ::mkdir(dir.c_str(), 0755);
}

constexpr uint32_t kMinBlockLen = 74;
constexpr uint32_t kMaxBlockLen = 4u * 1000u * 1000u;

// pread the full requested span or fail; loops over short reads.
bool PreadFull(int fd, void* buf, size_t count, off_t offset) {
    auto* p = static_cast<char*>(buf);
    while (count > 0) {
        ssize_t n = ::pread(fd, p, count, offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;  // unexpected EOF
        p += n;
        offset += n;
        count -= static_cast<size_t>(n);
    }
    return true;
}

std::string ReadWholeFile(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw std::runtime_error("cannot open " + path + ": " +
                                 std::strerror(errno));
    }
    off_t size = ::lseek(fd, 0, SEEK_END);
    if (size < 0) {
        ::close(fd);
        throw std::runtime_error("lseek failed on " + path);
    }
    std::string out;
    out.resize(static_cast<size_t>(size));
    if (size > 0 && !PreadFull(fd, out.data(), out.size(), 0)) {
        ::close(fd);
        throw std::runtime_error("read failed on " + path);
    }
    ::close(fd);
    return out;
}

}  // namespace

// FNV-1a 64-bit. Standard parameters; hashes height (uint64 LE) THEN data,
// matching Go's checksum(height, b) in common/cache.go.
uint64_t CacheChecksum(uint64_t height, const std::string& data) {
    constexpr uint64_t kOffsetBasis = 14695981039346656037ull;
    constexpr uint64_t kPrime = 1099511628211ull;
    uint64_t h = kOffsetBasis;
    // height as 8 bytes little-endian, then the protobuf bytes.
    unsigned char hbytes[8];
    for (int i = 0; i < 8; ++i) {
        hbytes[i] = static_cast<unsigned char>((height >> (8 * i)) & 0xff);
    }
    for (unsigned char b : hbytes) {
        h ^= b;
        h *= kPrime;
    }
    for (size_t i = 0; i < data.size(); ++i) {
        h ^= static_cast<unsigned char>(data[i]);
        h *= kPrime;
    }
    return h;
}

std::unique_ptr<BlockCache> BlockCache::Open(const std::string& data_dir,
                                             const std::string& chain,
                                             bool writable) {
    const std::string dir = data_dir + "/db/" + chain + "/";
    const std::string lengths_path = dir + "lengths";
    const std::string blocks_path = dir + "blocks";

    auto cache = std::unique_ptr<BlockCache>(new BlockCache());
    cache->chain_ = chain;
    cache->writable_ = writable;

    if (writable) {
        MakeDirs(dir);
        cache->blocks_fd_ = ::open(blocks_path.c_str(), O_RDWR | O_CREAT, 0644);
        if (cache->blocks_fd_ >= 0) {
            cache->lengths_fd_ =
                ::open(lengths_path.c_str(), O_RDWR | O_CREAT, 0644);
        }
        if (cache->blocks_fd_ < 0 || cache->lengths_fd_ < 0) {
            throw std::runtime_error("cannot open cache for writing in " + dir +
                                     ": " + std::strerror(errno));
        }
    } else {
        cache->blocks_fd_ = ::open(blocks_path.c_str(), O_RDONLY);
        if (cache->blocks_fd_ < 0) {
            throw std::runtime_error("cannot open " + blocks_path + ": " +
                                     std::strerror(errno));
        }
    }

    // Walk the lengths file to build starts_[]: one entry per block plus a
    // trailing sentinel (cumulative end offset), exactly like the Go reader.
    std::string lengths = ReadWholeFile(lengths_path);
    if (lengths.size() % 4 != 0) {
        throw std::runtime_error("lengths file size not a multiple of 4");
    }
    const size_t n_blocks = lengths.size() / 4;

    cache->lengths_.reserve(n_blocks);
    cache->starts_.reserve(n_blocks + 1);
    uint64_t offset = 0;
    cache->starts_.push_back(0);
    for (size_t i = 0; i < n_blocks; ++i) {
        const auto* p =
            reinterpret_cast<const unsigned char*>(lengths.data() + i * 4);
        uint32_t length = static_cast<uint32_t>(p[0]) |
                          (static_cast<uint32_t>(p[1]) << 8) |
                          (static_cast<uint32_t>(p[2]) << 16) |
                          (static_cast<uint32_t>(p[3]) << 24);
        if (length < kMinBlockLen || length > kMaxBlockLen) {
            throw std::runtime_error("lengths file has impossible value " +
                                     std::to_string(length) + " at index " +
                                     std::to_string(i));
        }
        cache->lengths_.push_back(length);
        offset += static_cast<uint64_t>(length) + 8;
        cache->starts_.push_back(offset);
    }

    // Sanity: blocks file must be at least as large as the computed total.
    off_t blocks_size = ::lseek(cache->blocks_fd_, 0, SEEK_END);
    if (blocks_size < 0 ||
        static_cast<uint64_t>(blocks_size) < offset) {
        throw std::runtime_error("blocks file shorter than lengths imply");
    }

    // Verify the first block parses (catches a stale/incompatible cache dir),
    // matching the Go reader's i==0 check.
    if (n_blocks > 0) {
        std::string raw;
        if (!cache->ReadRaw(0, raw)) {
            throw std::runtime_error(
                "first block failed checksum/read — incompatible cache dir");
        }
        // Tip hash is needed by the ingestor's HashMatch. Read the tip block's
        // CompactBlock.hash (wire order).
        std::string tip_raw;
        if (cache->ReadRaw(n_blocks - 1, tip_raw)) {
            rpc::CompactBlock tip;
            if (tip.ParseFromString(tip_raw)) {
                cache->latest_hash_ = tip.hash();
            }
        }
    }
    return cache;
}

BlockCache::~BlockCache() {
    if (lengths_fd_ >= 0) ::close(lengths_fd_);
    if (blocks_fd_ >= 0) ::close(blocks_fd_);
}

uint64_t BlockCache::Count() const {
    std::shared_lock lock(mu_);
    return lengths_.size();
}

std::optional<uint64_t> BlockCache::LatestHeight() const {
    std::shared_lock lock(mu_);
    if (lengths_.empty()) return std::nullopt;
    return lengths_.size() - 1;
}

bool BlockCache::ReadRaw(uint64_t height, std::string& out) const {
    // Caller holds at least the shared lock and has bounds-checked height,
    // EXCEPT Open() calls this single-threaded before publishing the cache.
    const uint32_t blockLen = lengths_[height];
    const uint64_t offset = starts_[height];

    std::string buf;
    buf.resize(static_cast<size_t>(blockLen) + 8);
    if (!PreadFull(blocks_fd_, buf.data(), buf.size(),
                   static_cast<off_t>(offset))) {
        return false;
    }

    // First 8 bytes = stored checksum (Go's fnv Sum(nil) is BIG-endian).
    const auto* cs = reinterpret_cast<const unsigned char*>(buf.data());
    uint64_t disk_cs = 0;
    for (int i = 0; i < 8; ++i) {
        disk_cs = (disk_cs << 8) | cs[i];
    }
    out.assign(buf.data() + 8, blockLen);
    if (CacheChecksum(height, out) != disk_cs) {
        return false;
    }
    return true;
}

std::optional<rpc::CompactBlock> BlockCache::Get(uint64_t height) const {
    std::shared_lock lock(mu_);
    if (height >= lengths_.size()) {
        return std::nullopt;
    }
    std::string raw;
    if (!ReadRaw(height, raw)) {
        throw std::runtime_error("cache corruption at height " +
                                 std::to_string(height));
    }
    rpc::CompactBlock block;
    if (!block.ParseFromString(raw)) {
        throw std::runtime_error("protobuf parse failed at height " +
                                 std::to_string(height));
    }
    if (block.height() != height) {
        throw std::runtime_error("block height mismatch at height " +
                                 std::to_string(height));
    }
    return block;
}

// ---- Write path ----

std::string BlockCache::LatestHash() const {
    std::shared_lock lock(mu_);
    return latest_hash_;
}

bool BlockCache::HashMatch(const std::string& prevhash) const {
    std::shared_lock lock(mu_);
    return latest_hash_.empty() || latest_hash_ == prevhash;
}

void BlockCache::AppendRecordLocked(uint64_t height, const std::string& data,
                                    const std::string& hash) {
    if (data.size() < kMinBlockLen || data.size() > kMaxBlockLen) {
        throw std::runtime_error("cache append length out of bounds: " +
                                 std::to_string(data.size()));
    }

    // Record = [8-byte BIG-endian FNV checksum][protobuf]. Append at the
    // current end offset (starts_.back()).
    const uint64_t offset = starts_.back();
    const uint64_t cs = CacheChecksum(height, data);
    std::string record;
    record.resize(8 + data.size());
    for (int i = 0; i < 8; ++i) {
        record[i] = static_cast<char>((cs >> (8 * (7 - i))) & 0xff);
    }
    std::memcpy(record.data() + 8, data.data(), data.size());
    if (!PwriteFull(blocks_fd_, record.data(), record.size(),
                    static_cast<off_t>(offset))) {
        throw std::runtime_error("cache append blocks write failed");
    }

    // lengths entry = uint32 LE protobuf length (excluding the 8 checksum bytes).
    unsigned char lb[4];
    const uint32_t len32 = static_cast<uint32_t>(data.size());
    lb[0] = len32 & 0xff;
    lb[1] = (len32 >> 8) & 0xff;
    lb[2] = (len32 >> 16) & 0xff;
    lb[3] = (len32 >> 24) & 0xff;
    if (!PwriteFull(lengths_fd_, lb, 4, static_cast<off_t>(4 * height))) {
        throw std::runtime_error("cache append lengths write failed");
    }

    lengths_.push_back(len32);
    starts_.push_back(offset + 8 + data.size());
    latest_hash_ = hash;
}

void BlockCache::Add(uint64_t height, const rpc::CompactBlock& block) {
    if (!writable_) {
        throw std::runtime_error("Add() on a read-only cache");
    }
    std::unique_lock lock(mu_);

    if (height != lengths_.size()) {
        throw std::runtime_error(
            "cache.Add out-of-order height: got " + std::to_string(height) +
            " expected " + std::to_string(lengths_.size()));
    }
    if (block.height() != height) {
        throw std::runtime_error("cache.Add wrong block height: block says " +
                                 std::to_string(block.height()));
    }

    std::string data;
    if (!block.SerializeToString(&data)) {
        throw std::runtime_error("cache.Add proto serialize failed");
    }
    AppendRecordLocked(height, data, block.hash());
}

void BlockCache::AddRaw(uint64_t height, const std::string& proto) {
    if (!writable_) {
        throw std::runtime_error("AddRaw() on a read-only cache");
    }
    std::unique_lock lock(mu_);

    if (height != lengths_.size()) {
        throw std::runtime_error(
            "cache.AddRaw out-of-order height: got " + std::to_string(height) +
            " expected " + std::to_string(lengths_.size()));
    }
    // Parse the (small) compact proto only for metadata: height check, prevHash
    // continuity, and the tip hash. The original bytes are stored verbatim.
    rpc::CompactBlock cb;
    if (!cb.ParseFromString(proto)) {
        throw std::runtime_error("cache.AddRaw proto parse failed at height " +
                                 std::to_string(height));
    }
    if (cb.height() != height) {
        throw std::runtime_error("cache.AddRaw wrong block height: block says " +
                                 std::to_string(cb.height()));
    }
    if (!latest_hash_.empty() && latest_hash_ != cb.prevhash()) {
        throw std::runtime_error("cache.AddRaw prevHash break at height " +
                                 std::to_string(height));
    }
    AppendRecordLocked(height, proto, cb.hash());
}

void BlockCache::Reorg(uint64_t height) {
    if (!writable_) {
        throw std::runtime_error("Reorg() on a read-only cache");
    }
    std::unique_lock lock(mu_);
    if (height >= lengths_.size()) return;  // nothing to drop

    lengths_.resize(height);
    starts_.resize(height + 1);  // keep the leading 0 + offsets up to `height`
    if (::ftruncate(lengths_fd_, static_cast<off_t>(4 * height)) != 0) {
        throw std::runtime_error("cache.Reorg lengths truncate failed");
    }
    if (::ftruncate(blocks_fd_, static_cast<off_t>(starts_[height])) != 0) {
        throw std::runtime_error("cache.Reorg blocks truncate failed");
    }
    // Recompute tip hash from the new tip block.
    latest_hash_.clear();
    if (height > 0) {
        std::string tip_raw;
        if (ReadRaw(height - 1, tip_raw)) {
            rpc::CompactBlock tip;
            if (tip.ParseFromString(tip_raw)) latest_hash_ = tip.hash();
        }
    }
}

void BlockCache::Sync() {
    if (!writable_) return;
    ::fsync(blocks_fd_);
    ::fsync(lengths_fd_);
}

}  // namespace lyghtd
