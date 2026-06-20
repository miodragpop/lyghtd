#include "rpc_client.h"

#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

#include <curl/curl.h>

#include "hex_bytes.h"

namespace lyghtd {

namespace {

size_t WriteCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// Trim ASCII whitespace from both ends.
std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

}  // namespace

RpcClient RpcClient::FromConf(const std::string& conf_path,
                              const std::string& host) {
    std::ifstream f(conf_path);
    if (!f) {
        throw std::runtime_error("cannot open conf file: " + conf_path);
    }
    std::string user, pass;
    int port = 8232;  // zcashd default; Ycash sets rpcport=12345 in conf
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        if (key == "rpcuser") user = val;
        else if (key == "rpcpassword") pass = val;
        else if (key == "rpcport") {
            try { port = std::stoi(val); } catch (...) {}
        }
    }
    if (user.empty() || pass.empty()) {
        throw std::runtime_error("conf missing rpcuser/rpcpassword: " + conf_path);
    }
    return RpcClient(host, port, std::move(user), std::move(pass));
}

RpcClient::RpcClient(std::string host, int port, std::string user,
                     std::string pass)
    : host_(std::move(host)),
      port_(port),
      user_(std::move(user)),
      pass_(std::move(pass)) {
    url_ = "http://" + host_ + ":" + std::to_string(port_) + "/";
    userpwd_ = user_ + ":" + pass_;
    curl_ = curl_easy_init();
    if (!curl_) throw std::runtime_error("curl_easy_init failed");
    ConfigureHandle();
}

void RpcClient::ConfigureHandle() {
    auto* curl = static_cast<CURL*>(curl_);
    // Constant request header (set once; the slist lives for the handle's life).
    auto* headers = curl_slist_append(nullptr, "content-type: text/plain;");
    headers_ = headers;

    // Connection-invariant options — set ONCE here, not per request. The handle
    // keeps its connection cache across calls, so subsequent RawRequest()s reuse
    // the same TCP connection (HTTP/1.1 keep-alive).
    curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_USERPWD, userpwd_.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 1024L * 1024L);
    // NOTE: a single reused connection stalls ~40ms/request on TCP delayed-ACK
    // against ycashd's RPC socket (which has Nagle on); a fresh connection per
    // request avoids the stall but exhausts ephemeral ports at scale. The fix is
    // to drive a POOL of these long-lived connections in parallel (see the
    // ingestor), which overlaps the stalls and reaches ycashd's ceiling.
}

RpcClient::~RpcClient() {
    if (curl_) curl_easy_cleanup(static_cast<CURL*>(curl_));
    if (headers_) curl_slist_free_all(static_cast<curl_slist*>(headers_));
}

RpcClient::RpcClient(RpcClient&& o) noexcept
    : host_(std::move(o.host_)),
      port_(o.port_),
      user_(std::move(o.user_)),
      pass_(std::move(o.pass_)),
      url_(std::move(o.url_)),
      userpwd_(std::move(o.userpwd_)),
      curl_(o.curl_),
      headers_(o.headers_),
      id_(o.id_) {
    o.curl_ = nullptr;
    o.headers_ = nullptr;
}

namespace {

// Parse only the fields we use; error_on_unknown_keys=false skips the rest
// (the bulk of the verbose getblock object is never materialized).
constexpr glz::opts kReadOpts{.error_on_unknown_keys = false};

// JSON-RPC error object.
struct RpcErrJson {
    int code = 0;
    std::string message{};
};

// JSON-RPC envelope. On success result is present and error is null; on failure
// the reverse (std::optional handles JSON null cleanly).
template <class T>
struct ReplyJson {
    std::optional<T> result{};
    std::optional<RpcErrJson> error{};
};

// getblock verbosity-1 result.
struct TreeSizeJson { uint32_t size = 0; };
struct TreesJson { TreeSizeJson sapling{}; TreeSizeJson orchard{}; };
struct VerboseJson {
    std::string hash{};
    std::vector<std::string> tx{};
    TreesJson trees{};
};

// getblockchaininfo result.
struct ConsensusJson { std::string chaintip{}; };
struct UpgradeJson {
    std::string name{};
    std::string status{};
    uint64_t activationheight = 0;
};
struct ChainInfoJson {
    uint64_t blocks = 0;
    std::optional<uint64_t> estimatedheight{};
    std::string chain{};
    ConsensusJson consensus{};
    std::map<std::string, UpgradeJson> upgrades{};
};

// getinfo result.
struct InfoJson {
    std::string build{};
    std::string subversion{};
};

// One element of a JSON-RPC batch response. The result is captured as raw JSON
// text (deferred parse — no DOM of the whole batch); id routes it to its slot.
struct BatchElem {
    std::string id{};
    glz::raw_json result{};
    std::optional<RpcErrJson> error{};
};

// Batch element for the all-raw path: the result (a JSON hex string) is decoded
// straight to binary by the HexBytes reader during parse — no hex string.
struct RawBatchElem {
    std::string id{};
    std::optional<HexBytes> result{};
    std::optional<RpcErrJson> error{};
};

// Parse a single-call envelope into result T (throws on JSON/RPC error).
template <class T>
T ParseReply(const char* method, const std::string& resp) {
    ReplyJson<T> r;
    if (glz::read<kReadOpts>(r, resp)) {
        throw std::runtime_error(std::string("RPC ") + method +
                                 ": bad JSON reply");
    }
    if (r.error) {
        throw std::runtime_error(std::string("RPC ") + method +
                                 " error: " + r.error->message + " (code " +
                                 std::to_string(r.error->code) + ")");
    }
    if (!r.result) {
        throw std::runtime_error(std::string("RPC ") + method + ": null result");
    }
    return std::move(*r.result);
}

VerboseBlock ToVerboseBlock(VerboseJson&& j) {
    VerboseBlock vb;
    vb.hash = std::move(j.hash);
    vb.tx = std::move(j.tx);
    vb.sapling_tree_size = j.trees.sapling.size;
    vb.orchard_tree_size = j.trees.orchard.size;
    return vb;
}

}  // namespace

std::string RpcClient::HttpPost(const std::string& body) {
    auto* curl = static_cast<CURL*>(curl_);
    std::string resp;
    // Only the mutable per-request options change; the rest were set once in
    // ConfigureHandle().
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("RPC transport error: ") +
                                 curl_easy_strerror(rc));
    }
    // curl's own measured transfer time (connect+send+wait+receive), so callers
    // can separate server/transport latency from our JSON handling.
    double tt = 0;
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &tt);
    last_transfer_seconds_ = tt;
    return resp;
}

namespace {
// Build a single JSON-RPC request body.
std::string SingleBody(uint64_t id, const std::string& method,
                       const std::string& params_json) {
    return R"({"jsonrpc":"1.0","id":")" + std::to_string(id) +
           R"(","method":")" + method + R"(","params":)" + params_json + "}";
}
}  // namespace

std::string RpcClient::RawRequest(const std::string& method,
                                  const std::string& params_json) {
    std::string resp;
    {
        std::lock_guard<std::mutex> lock(mu_);
        resp = HttpPost(SingleBody(++id_, method, params_json));
    }
    // Return the result as raw JSON text (no DOM, no reserialize round-trip).
    ReplyJson<glz::raw_json> r;
    if (glz::read<kReadOpts>(r, resp)) {
        throw std::runtime_error("RPC " + method + ": bad JSON reply");
    }
    if (r.error) {
        throw std::runtime_error("RPC " + method + " error: " + r.error->message);
    }
    if (!r.result) {
        throw std::runtime_error("RPC " + method + ": null result");
    }
    return std::move(r.result->str);
}

std::vector<std::string> RpcClient::RawRequestBatch(
    const std::vector<std::pair<std::string, std::string>>& calls) {
    // id is the request index, so we can place results back in order even if the
    // server reorders the responses.
    std::string body = "[";
    for (size_t i = 0; i < calls.size(); ++i) {
        if (i) body += ",";
        body += SingleBody(i, calls[i].first, calls[i].second);
    }
    body += "]";

    std::string resp;
    {
        std::lock_guard<std::mutex> lock(mu_);
        resp = HttpPost(body);
    }

    // Each element's result is captured as raw JSON text (no DOM); id routes it
    // to the right slot.
    std::vector<BatchElem> elems;
    if (glz::read<kReadOpts>(elems, resp)) {
        throw std::runtime_error("RPC batch: bad JSON reply");
    }
    if (elems.size() != calls.size()) {
        throw std::runtime_error("RPC batch: response count mismatch");
    }
    std::vector<std::string> results(calls.size());
    std::vector<bool> filled(calls.size(), false);
    for (auto& el : elems) {
        size_t idx = static_cast<size_t>(std::stoul(el.id));
        if (idx >= calls.size()) {
            throw std::runtime_error("RPC batch: bad response id");
        }
        if (el.error) {
            throw std::runtime_error("RPC batch " + calls[idx].first +
                                     " error: " + el.error->message + " (code " +
                                     std::to_string(el.error->code) + ")");
        }
        results[idx] = std::move(el.result.str);
        filled[idx] = true;
    }
    for (bool f : filled) {
        if (!f) throw std::runtime_error("RPC batch: missing a response id");
    }
    return results;
}

std::string RpcClient::GetBestBlockHash() {
    std::string resp;
    {
        std::lock_guard<std::mutex> lock(mu_);
        resp = HttpPost(SingleBody(++id_, "getbestblockhash", "[]"));
    }
    return ParseReply<std::string>("getbestblockhash", resp);
}

ChainInfo RpcClient::GetBlockChainInfo() {
    std::string resp;
    {
        std::lock_guard<std::mutex> lock(mu_);
        resp = HttpPost(SingleBody(++id_, "getblockchaininfo", "[]"));
    }
    ChainInfoJson j = ParseReply<ChainInfoJson>("getblockchaininfo", resp);

    ChainInfo ci;
    ci.blocks = j.blocks;
    ci.estimated_height = j.estimatedheight.value_or(j.blocks);
    ci.chain = std::move(j.chain);
    ci.consensus_chaintip = std::move(j.consensus.chaintip);
    // Sapling activation height + the lowest-height pending upgrade.
    bool have_pending = false;
    for (const auto& [id, u] : j.upgrades) {
        if (id == "76b809bb") {  // Sapling
            ci.sapling_activation_height = u.activationheight;
        }
        // NU5/Orchard: keyed by upgrade NAME, not a hardcoded branch-id (Ycash's
        // branch ids differ from Zcash's). Absent on chains without NU5.
        if (u.name == "NU5") {
            ci.nu5_activation_height = u.activationheight;
        }
        if (u.status == "pending" &&
            (!have_pending || u.activationheight < ci.upgrade_height)) {
            have_pending = true;
            ci.upgrade_height = u.activationheight;
            ci.upgrade_name = u.name;
        }
    }
    return ci;
}

DaemonInfo RpcClient::GetInfo() {
    std::string resp;
    {
        std::lock_guard<std::mutex> lock(mu_);
        resp = HttpPost(SingleBody(++id_, "getinfo", "[]"));
    }
    InfoJson j = ParseReply<InfoJson>("getinfo", resp);
    return {std::move(j.build), std::move(j.subversion)};
}

VerboseBlock RpcClient::GetBlockVerbose(uint64_t height) {
    std::string params = "[\"" + std::to_string(height) + "\",1]";
    std::string resp;
    {
        std::lock_guard<std::mutex> lock(mu_);
        resp = HttpPost(SingleBody(++id_, "getblock", params));
    }
    return ToVerboseBlock(ParseReply<VerboseJson>("getblock verbose", resp));
}

std::string RpcClient::GetBlockRawHex(const std::string& hash) {
    std::string params = "[\"" + hash + "\",0]";
    std::string resp;
    {
        std::lock_guard<std::mutex> lock(mu_);
        resp = HttpPost(SingleBody(++id_, "getblock", params));
    }
    return ParseReply<std::string>("getblock raw", resp);
}

RpcClient::BlockFetch RpcClient::GetBlockBatched(uint64_t height) {
    return std::move(GetBlocksBatched(height, 1)[0]);
}

std::vector<RpcClient::BlockFetch> RpcClient::GetBlocksBatched(
    uint64_t start_height, uint64_t count) {
    // 2 calls per block (verbose + raw) in one request. id = call index, so
    // block i's verbose is id 2*i (even), its raw id 2*i+1 (odd).
    std::string body = "[";
    for (uint64_t i = 0; i < count; ++i) {
        const std::string h = std::to_string(start_height + i);
        if (i) body += ",";
        body += SingleBody(2 * i, "getblock", "[\"" + h + "\",1]") + "," +
                SingleBody(2 * i + 1, "getblock", "[\"" + h + "\",0]");
    }
    body += "]";

    std::string resp;
    {
        std::lock_guard<std::mutex> lock(mu_);
        resp = HttpPost(body);
    }

    // Capture each result as raw JSON text (no DOM of the whole batch), then
    // parse only what we need: verbose → small struct, raw → the hex string.
    std::vector<BatchElem> elems;
    if (glz::read<kReadOpts>(elems, resp)) {
        throw std::runtime_error("getblock batch: bad JSON reply");
    }
    if (elems.size() != count * 2) {
        throw std::runtime_error("getblock batch: response count mismatch");
    }
    std::vector<BlockFetch> out(count);
    std::vector<bool> filled(count * 2, false);
    for (auto& el : elems) {
        size_t idx = static_cast<size_t>(std::stoul(el.id));
        if (idx >= count * 2) {
            throw std::runtime_error("getblock batch: bad response id");
        }
        if (el.error) {
            throw std::runtime_error("getblock batch error: " +
                                     el.error->message + " (code " +
                                     std::to_string(el.error->code) + ")");
        }
        const size_t blk = idx / 2;
        if (idx % 2 == 0) {  // verbose
            VerboseJson vj;
            if (glz::read<kReadOpts>(vj, el.result.str)) {
                throw std::runtime_error("getblock verbose (batch): bad result");
            }
            out[blk].verbose = ToVerboseBlock(std::move(vj));
        } else {  // raw hex (a JSON string)
            if (glz::read_json(out[blk].raw_hex, el.result.str)) {
                throw std::runtime_error("getblock raw (batch): bad result");
            }
        }
        filled[idx] = true;
    }
    for (bool f : filled) {
        if (!f) throw std::runtime_error("getblock batch: missing a response id");
    }
    return out;
}

std::vector<std::string> RpcClient::GetBlocksRawBatched(uint64_t start_height,
                                                        uint64_t count) {
    // 1 call/block (raw, verbosity 0), homogeneous → parse straight into typed
    // replies; the HexBytes reader decodes each block's hex to binary in-parse.
    std::string body = "[";
    for (uint64_t i = 0; i < count; ++i) {
        const std::string h = std::to_string(start_height + i);
        if (i) body += ",";
        body += SingleBody(i, "getblock", "[\"" + h + "\",0]");
    }
    body += "]";

    std::string resp;
    {
        std::lock_guard<std::mutex> lock(mu_);
        resp = HttpPost(body);
    }

    std::vector<RawBatchElem> elems;
    if (glz::read<kReadOpts>(elems, resp)) {
        throw std::runtime_error("getblock raw batch: bad JSON reply");
    }
    if (elems.size() != count) {
        throw std::runtime_error("getblock raw batch: response count mismatch");
    }
    std::vector<std::string> out(count);
    std::vector<bool> filled(count, false);
    for (auto& el : elems) {
        size_t idx = static_cast<size_t>(std::stoul(el.id));
        if (idx >= count) {
            throw std::runtime_error("getblock raw batch: bad response id");
        }
        if (el.error) {
            throw std::runtime_error("getblock raw batch error: " +
                                     el.error->message + " (code " +
                                     std::to_string(el.error->code) + ")");
        }
        if (!el.result) {
            throw std::runtime_error("getblock raw batch: null result");
        }
        out[idx] = std::move(el.result->bytes);
        filled[idx] = true;
    }
    for (bool f : filled) {
        if (!f)
            throw std::runtime_error("getblock raw batch: missing a response id");
    }
    return out;
}

std::vector<std::string> RpcClient::GetExperimentalFeatures() {
    std::string resp;
    {
        std::lock_guard<std::mutex> lock(mu_);
        resp = HttpPost(SingleBody(++id_, "getexperimentalfeatures", "[]"));
    }
    return ParseReply<std::vector<std::string>>("getexperimentalfeatures", resp);
}

bool RpcClient::SupportsCompactBlocks() {
    try {
        for (const auto& f : GetExperimentalFeatures()) {
            if (f == "compactblocks") return true;
        }
    } catch (const std::exception&) {
        // getexperimentalfeatures is itself absent on older daemons → no feature.
    }
    return false;
}

std::vector<std::string> RpcClient::GetCompactBlockRange(uint64_t start,
                                                         uint64_t count) {
    // Single call; result is a JSON array of hex-encoded CompactBlock protos.
    // The HexBytes reader decodes each straight to binary during the parse.
    const std::string params =
        "[" + std::to_string(start) + "," + std::to_string(count) + "]";
    std::string resp;
    {
        std::lock_guard<std::mutex> lock(mu_);
        resp = HttpPost(SingleBody(++id_, "getcompactblockrange", params));
    }
    std::vector<HexBytes> elems =
        ParseReply<std::vector<HexBytes>>("getcompactblockrange", resp);
    if (elems.size() != count) {
        throw std::runtime_error("getcompactblockrange: response count mismatch (got " +
                                 std::to_string(elems.size()) + " want " +
                                 std::to_string(count) + ")");
    }
    std::vector<std::string> out(count);
    for (uint64_t i = 0; i < count; ++i) out[i] = std::move(elems[i].bytes);
    return out;
}

}  // namespace lyghtd
