#pragma once

// RpcClient — minimal ycashd JSON-RPC client over plaintext HTTP (Bitcoin-style:
// HTTP POST + basic auth, body {"jsonrpc","id","method","params"}). Built for the
// M2 ingestor's needs only: fetch blocks (verbose + raw) and chain info. Reuses
// the libcurl + glaze patterns proven in inzyght's ycash_rpc_client, without its
// explorer-specific surface.
//
// Single-threaded use (the ingestor): one reused CURL handle for keep-alive.

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>

namespace lyghtd {

// Parsed subset of getblock <height> 1 (verbose). Field names match the JSON.
struct VerboseBlock {
    std::string hash;             // block hash, big-endian display hex
    std::vector<std::string> tx;  // txids, big-endian display hex
    uint32_t sapling_tree_size = 0;
    uint32_t orchard_tree_size = 0;
};

// Parsed subset of getblockchaininfo.
struct ChainInfo {
    uint64_t blocks = 0;
    uint64_t estimated_height = 0;
    std::string chain;
    std::string consensus_chaintip;  // consensus.chaintip, e.g. "19bd2d2f"
    uint64_t sapling_activation_height = 0;
    std::string upgrade_name;        // next pending upgrade, "" if none
    uint64_t upgrade_height = 0;
    // NU5/Orchard activation height, or UINT64_MAX if this chain has no NU5
    // upgrade (e.g. Ycash). Below it, V5 txids are computable as SHA256d.
    uint64_t nu5_activation_height = UINT64_MAX;
};

// Parsed subset of getinfo.
struct DaemonInfo {
    std::string build;
    std::string subversion;
};

class RpcClient {
public:
    // Build a client from a ycashd/zcashd conf file (rpcuser/rpcpassword/rpcport).
    // Throws std::runtime_error if the file can't be read or creds are missing.
    static RpcClient FromConf(const std::string& conf_path,
                              const std::string& host = "127.0.0.1");

    RpcClient(std::string host, int port, std::string user, std::string pass);
    ~RpcClient();

    RpcClient(RpcClient&&) noexcept;
    RpcClient& operator=(RpcClient&&) = delete;
    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;

    // Core call: send `method` with `params_json` (a JSON array literal, e.g.
    // R"(["abc",0])" or "[]"), return the raw "result" sub-JSON as a string.
    // Throws std::runtime_error on transport failure or a JSON-RPC error reply.
    std::string RawRequest(const std::string& method,
                           const std::string& params_json);

    // JSON-RPC batch: send all (method, params_json) calls in ONE HTTP request
    // (one round trip) and return each call's "result" JSON in request order.
    // Throws on transport failure or if any sub-call returns an error.
    std::vector<std::string> RawRequestBatch(
        const std::vector<std::pair<std::string, std::string>>& calls);

    // Typed helpers (mirror the Go ingestor's RPC usage).
    std::string GetBestBlockHash();
    ChainInfo GetBlockChainInfo();
    DaemonInfo GetInfo();
    VerboseBlock GetBlockVerbose(uint64_t height);          // getblock <h> 1
    std::string GetBlockRawHex(const std::string& hash);    // getblock <hash> 0

    // Both getblock calls for one height in a single batch (one round trip):
    // verbose (hash/txids/tree-sizes) + raw bytes. Raw is fetched by height,
    // which is safe here since both run back-to-back in one request.
    struct BlockFetch {
        VerboseBlock verbose;
        std::string raw_hex;
    };
    BlockFetch GetBlockBatched(uint64_t height);

    // Fetch `count` consecutive blocks [start_height, start_height+count) in ONE
    // batched request (2*count getblock calls: verbose + raw per block),
    // returned in height order. Amortizes the round trip across `count` blocks.
    std::vector<BlockFetch> GetBlocksBatched(uint64_t start_height,
                                             uint64_t count);

    // Fetch `count` consecutive RAW blocks [start_height, start_height+count) in
    // ONE batched request (1 getblock call/block, verbosity 0), returned as
    // BINARY block bytes in height order. The hex is decoded straight from the
    // response during JSON parsing (no intermediate hex string). Used when the
    // range is below the NU5 height, where the parser computes txids itself.
    std::vector<std::string> GetBlocksRawBatched(uint64_t start_height,
                                                 uint64_t count);

    // Enabled experimental features (getexperimentalfeatures), e.g.
    // {"lightwalletd","compactblocks"}. Used to probe for the compact-block RPCs.
    std::vector<std::string> GetExperimentalFeatures();

    // True if this daemon advertises the "compactblocks" experimental feature
    // (getcompactblock/getcompactblockrange available). Probed once at startup.
    bool SupportsCompactBlocks();

    // getcompactblockrange start count — fetch `count` consecutive blocks
    // [start, start+count) already built into serialized CompactBlock protobufs
    // by the daemon, returned as BINARY proto bytes in height order (decoded from
    // hex straight during JSON parse). The single-call fast path: ycashd does the
    // block->CompactBlock work, so the bytes are stored verbatim. count <= 10000.
    std::vector<std::string> GetCompactBlockRange(uint64_t start, uint64_t count);

    const std::string& host() const { return host_; }
    int port() const { return port_; }

    // curl's measured transfer time of the most recent RawRequest, in seconds
    // (connect+send+server-wait+receive). Lets callers separate transport/server
    // latency from our own JSON handling. Not thread-safe across callers.
    double last_transfer_seconds() const { return last_transfer_seconds_; }

private:
    std::string host_;
    int port_;
    std::string user_;
    std::string pass_;
    std::string url_;
    std::string userpwd_;
    void* curl_ = nullptr;     // CURL*; void* to keep curl out of the header
    void* headers_ = nullptr;  // curl_slist*; constant request headers
    std::mutex mu_;
    uint64_t id_ = 0;
    double last_transfer_seconds_ = 0;  // curl CURLINFO_TOTAL_TIME of last call

    // Set the connection-invariant curl options (URL, auth, write cb, timeout,
    // headers) once. Per-request, only POSTFIELDS/size/WRITEDATA change.
    void ConfigureHandle();

    // POST `body` and return the raw response. Caller must hold mu_.
    std::string HttpPost(const std::string& body);
};

}  // namespace lyghtd
