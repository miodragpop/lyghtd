#include "block_parser.h"

#include <atomic>
#include <cstdint>
#include <stdexcept>

#include "bytestring.h"
#include "sha256.h"

namespace lyghtd {

namespace {

// ---- transaction version constants (mirror Go transaction.go) ----
constexpr uint32_t OVERWINTER_TX_VERSION = 3;
constexpr uint32_t SAPLING_TX_VERSION = 4;
constexpr uint32_t ZIP225_TX_VERSION = 5;
constexpr uint32_t OVERWINTER_VERSION_GROUP_ID = 0x03C48270;
constexpr uint32_t SAPLING_VERSION_GROUP_ID = 0x892F2085;
constexpr uint32_t ZIP225_VERSION_GROUP_ID = 0x26A7270A;

// NU5 (ZIP225/Orchard) activation height; V5 parsing is enabled at/above it.
// UINT64_MAX = never (no NU5, e.g. Ycash).
std::atomic<uint64_t> g_nu5_height{UINT64_MAX};

// SHA256d of `bytes`, returned as a 32-byte std::string in wire order — the form
// CompactBlock stores for block hash and txids.
std::string Sha256dStr(std::string_view bytes) {
    uint8_t out[32];
    SHA256d(bytes.data(), bytes.size(), out);
    return std::string(reinterpret_cast<const char*>(out), 32);
}

[[noreturn]] void Fail(const char* what) {
    throw std::runtime_error(std::string("block parse: ") + what);
}

// Decode big-endian display hex (e.g. a txid/blockhash) to raw bytes, then
// reverse to wire (little-endian) order — the form CompactBlock stores.
std::string DisplayHexToWire(const std::string& hex) {
    if (hex.size() % 2 != 0) Fail("odd-length hex");
    const size_t n = hex.size() / 2;
    std::string out(n, '\0');
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; ++i) {
        int hi = nib(hex[2 * i]), lo = nib(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) Fail("bad hex digit");
        // write reversed: byte i of input goes to position n-1-i of output
        out[n - 1 - i] = static_cast<char>((hi << 4) | lo);
    }
    return out;
}

// ---- transaction parse state ----
struct Spend {
    std::string nullifier;  // 32
};
struct Output {
    std::string cmu;            // 32
    std::string ephemeralKey;   // 32
    std::string encCiphertext;  // 580 (compact stores first 52)
};
struct Action {
    std::string nullifier;      // 32
    std::string cmx;            // 32
    std::string ephemeralKey;   // 32
    std::string encCiphertext;  // 580 (compact stores first 52)
};
struct TxIn {
    std::string prevTxHash;  // 32 (wire order, as on disk)
    uint32_t prevTxOutIndex = 0;
    std::string scriptSig;
};
struct TxOut {
    uint64_t value = 0;
    std::string script;
};

struct Tx {
    bool fOverwintered = false;
    uint32_t version = 0;
    uint32_t nVersionGroupID = 0;
    uint32_t consensusBranchID = 0;
    std::vector<TxIn> tin;
    std::vector<TxOut> tout;
    std::vector<Spend> spends;
    std::vector<Output> outputs;
    std::vector<Action> actions;

    bool isOverwinterV3() const {
        return fOverwintered &&
               nVersionGroupID == OVERWINTER_VERSION_GROUP_ID &&
               version == OVERWINTER_TX_VERSION;
    }
    bool isSaplingV4() const {
        return fOverwintered && nVersionGroupID == SAPLING_VERSION_GROUP_ID &&
               version == SAPLING_TX_VERSION;
    }
    bool isZip225V5() const {
        return fOverwintered && nVersionGroupID == ZIP225_VERSION_GROUP_ID &&
               version == ZIP225_TX_VERSION;
    }
    bool isGroth16Proof() const {
        return fOverwintered && version >= SAPLING_TX_VERSION;
    }
};

void ParseTransparent(ByteString& s, Tx& tx) {
    uint64_t txInCount;
    if (!s.ReadCompactSize(&txInCount)) Fail("tx_in_count");
    tx.tin.resize(txInCount);
    for (uint64_t i = 0; i < txInCount; ++i) {
        TxIn& ti = tx.tin[i];
        if (!s.ReadBytes(&ti.prevTxHash, 32)) Fail("prevTxHash");
        if (!s.ReadUint32(&ti.prevTxOutIndex)) Fail("prevTxOutIndex");
        if (!s.ReadCompactLengthPrefixed(&ti.scriptSig)) Fail("scriptSig");
        if (!s.Skip(4)) Fail("sequence");
    }
    uint64_t txOutCount;
    if (!s.ReadCompactSize(&txOutCount)) Fail("tx_out_count");
    tx.tout.resize(txOutCount);
    for (uint64_t i = 0; i < txOutCount; ++i) {
        TxOut& to = tx.tout[i];
        if (!s.ReadUint64(&to.value)) Fail("txOut value");
        if (!s.ReadCompactLengthPrefixed(&to.script)) Fail("txOut script");
    }
}

void ParseSpend(ByteString& s, Spend& sp, uint32_t version) {
    if (!s.Skip(32)) Fail("spend cv");
    if (version <= 4 && !s.Skip(32)) Fail("spend anchor");
    if (!s.ReadBytes(&sp.nullifier, 32)) Fail("spend nullifier");
    if (!s.Skip(32)) Fail("spend rk");
    if (version <= 4 && !s.Skip(192)) Fail("spend zkproof");
    if (version <= 4 && !s.Skip(64)) Fail("spend spendAuthSig");
}

void ParseOutput(ByteString& s, Output& o, uint32_t version) {
    if (!s.Skip(32)) Fail("output cv");
    if (!s.ReadBytes(&o.cmu, 32)) Fail("output cmu");
    if (!s.ReadBytes(&o.ephemeralKey, 32)) Fail("output ephemeralKey");
    if (!s.ReadBytes(&o.encCiphertext, 580)) Fail("output encCiphertext");
    if (!s.Skip(80)) Fail("output outCiphertext");
    if (version <= 4 && !s.Skip(192)) Fail("output zkproof");
}

void ParseJoinSplit(ByteString& s, bool isGroth16Proof) {
    if (!s.Skip(8)) Fail("js vpubOld");
    if (!s.Skip(8)) Fail("js vpubNew");
    if (!s.Skip(32)) Fail("js anchor");
    for (int i = 0; i < 2; ++i)
        if (!s.Skip(32)) Fail("js nullifier");
    for (int i = 0; i < 2; ++i)
        if (!s.Skip(32)) Fail("js commitment");
    if (!s.Skip(32)) Fail("js ephemeralKey");
    if (!s.Skip(32)) Fail("js randomSeed");
    for (int i = 0; i < 2; ++i)
        if (!s.Skip(32)) Fail("js vmac");
    if (isGroth16Proof) {
        if (!s.Skip(192)) Fail("js Groth16 proof");
    } else {
        if (!s.Skip(296)) Fail("js PHGR proof");
    }
    for (int i = 0; i < 2; ++i)
        if (!s.Skip(601)) Fail("js encCiphertext");
}

void ParseAction(ByteString& s, Action& a) {
    if (!s.Skip(32)) Fail("action cv");
    if (!s.ReadBytes(&a.nullifier, 32)) Fail("action nullifier");
    if (!s.Skip(32)) Fail("action rk");
    if (!s.ReadBytes(&a.cmx, 32)) Fail("action cmx");
    if (!s.ReadBytes(&a.ephemeralKey, 32)) Fail("action ephemeralKey");
    if (!s.ReadBytes(&a.encCiphertext, 580)) Fail("action encCiphertext");
    if (!s.Skip(80)) Fail("action outCiphertext");
}

void ParsePreV5(ByteString& s, Tx& tx) {
    ParseTransparent(s, tx);
    if (!s.Skip(4)) Fail("nLockTime");
    if (tx.version > 1) {
        if ((tx.isOverwinterV3() || tx.isSaplingV4()) && !s.Skip(4))
            Fail("nExpiryHeight");

        uint64_t spendCount = 0, outputCount = 0;
        if (tx.isSaplingV4()) {
            if (!s.Skip(8)) Fail("valueBalance");
            if (!s.ReadCompactSize(&spendCount)) Fail("nShieldedSpend");
            tx.spends.resize(spendCount);
            for (uint64_t i = 0; i < spendCount; ++i)
                ParseSpend(s, tx.spends[i], 4);
            if (!s.ReadCompactSize(&outputCount)) Fail("nShieldedOutput");
            tx.outputs.resize(outputCount);
            for (uint64_t i = 0; i < outputCount; ++i)
                ParseOutput(s, tx.outputs[i], tx.version);
        }

        uint64_t joinSplitCount = 0;
        if (!s.ReadCompactSize(&joinSplitCount)) Fail("nJoinSplit");
        if (joinSplitCount > 0) {
            for (uint64_t i = 0; i < joinSplitCount; ++i)
                ParseJoinSplit(s, tx.isGroth16Proof());
            if (!s.Skip(32)) Fail("joinSplitPubKey");
            if (!s.Skip(64)) Fail("joinSplitSig");
        }
        if (tx.isSaplingV4() && spendCount + outputCount > 0 && !s.Skip(64))
            Fail("bindingSigSapling");
    }
}

void ParseV5(ByteString& s, Tx& tx) {
    if (!s.ReadUint32(&tx.consensusBranchID)) Fail("v5 consensusBranchId");
    if (tx.nVersionGroupID != ZIP225_VERSION_GROUP_ID)
        Fail("v5 version group id");
    if (!s.Skip(4)) Fail("v5 nLockTime");
    if (!s.Skip(4)) Fail("v5 nExpiryHeight");
    ParseTransparent(s, tx);

    uint64_t spendCount = 0, outputCount = 0;
    if (!s.ReadCompactSize(&spendCount)) Fail("v5 nShieldedSpend");
    if (spendCount >= (1u << 16)) Fail("v5 spendCount too large");
    tx.spends.resize(spendCount);
    for (uint64_t i = 0; i < spendCount; ++i)
        ParseSpend(s, tx.spends[i], tx.version);
    if (!s.ReadCompactSize(&outputCount)) Fail("v5 nShieldedOutput");
    if (outputCount >= (1u << 16)) Fail("v5 outputCount too large");
    tx.outputs.resize(outputCount);
    for (uint64_t i = 0; i < outputCount; ++i)
        ParseOutput(s, tx.outputs[i], tx.version);
    if (spendCount + outputCount > 0 && !s.Skip(8)) Fail("v5 valueBalance");
    if (spendCount > 0 && !s.Skip(32)) Fail("v5 anchorSapling");
    if (!s.Skip(192 * spendCount)) Fail("v5 vSpendProofsSapling");
    if (!s.Skip(64 * spendCount)) Fail("v5 vSpendAuthSigsSapling");
    if (!s.Skip(192 * outputCount)) Fail("v5 vOutputProofsSapling");
    if (spendCount + outputCount > 0 && !s.Skip(64)) Fail("v5 bindingSigSapling");

    uint64_t actionsCount = 0;
    if (!s.ReadCompactSize(&actionsCount)) Fail("v5 nActionsOrchard");
    if (actionsCount >= (1u << 16)) Fail("v5 actionsCount too large");
    tx.actions.resize(actionsCount);
    for (uint64_t i = 0; i < actionsCount; ++i)
        ParseAction(s, tx.actions[i]);
    if (actionsCount > 0) {
        if (!s.Skip(1)) Fail("v5 flagsOrchard");
        if (!s.Skip(8)) Fail("v5 valueBalanceOrchard");
        if (!s.Skip(32)) Fail("v5 anchorOrchard");
        uint64_t proofsCount = 0;
        if (!s.ReadCompactSize(&proofsCount)) Fail("v5 sizeProofsOrchard");
        if (!s.Skip(proofsCount)) Fail("v5 proofsOrchard");
        if (!s.Skip(64 * actionsCount)) Fail("v5 vSpendAuthSigsOrchard");
        if (!s.Skip(64)) Fail("v5 bindingSigOrchard");
    }
}

// Parse one transaction starting at s; fills tx. `nu5_active` enables V5 parsing
// (true only for blocks at/above the NU5 activation height).
void ParseTransaction(ByteString& s, Tx& tx, bool nu5_active) {
    uint32_t header;
    if (!s.ReadUint32(&header)) Fail("tx header");
    tx.fOverwintered = (header >> 31) == 1;
    tx.version = header & 0x7FFFFFFF;
    if (tx.fOverwintered) {
        if (!s.ReadUint32(&tx.nVersionGroupID)) Fail("nVersionGroupId");
    }
    const bool v5OK = nu5_active && tx.isZip225V5();
    if (tx.fOverwintered &&
        !(tx.isOverwinterV3() || tx.isSaplingV4() || v5OK)) {
        Fail("unknown transaction format");
    }
    if (v5OK) {
        ParseV5(s, tx);
    } else {
        ParsePreV5(s, tx);
    }
}

// Convert a parsed Tx to its CompactTx (mirrors Transaction.ToCompact).
void ToCompactTx(const Tx& tx, int index, const std::string& wire_txid,
                 rpc::CompactTx* ctx) {
    ctx->set_index(static_cast<uint64_t>(index));
    ctx->set_txid(wire_txid);
    // Fee left unset (Go: TODO).
    for (const auto& sp : tx.spends) {
        ctx->add_spends()->set_nf(sp.nullifier);
    }
    for (const auto& o : tx.outputs) {
        auto* co = ctx->add_outputs();
        co->set_cmu(o.cmu);
        co->set_ephemeralkey(o.ephemeralKey);
        co->set_ciphertext(o.encCiphertext.substr(0, 52));
    }
    for (const auto& a : tx.actions) {
        auto* ca = ctx->add_actions();
        ca->set_nullifier(a.nullifier);
        ca->set_cmx(a.cmx);
        ca->set_ephemeralkey(a.ephemeralKey);
        ca->set_ciphertext(a.encCiphertext.substr(0, 52));
    }
    // Coinbase (index 0) does not store transparent inputs.
    if (index > 0) {
        for (const auto& ti : tx.tin) {
            auto* cv = ctx->add_vin();
            cv->set_prevouttxid(ti.prevTxHash);
            cv->set_prevoutindex(ti.prevTxOutIndex);
        }
    }
    for (const auto& to : tx.tout) {
        auto* cv = ctx->add_vout();
        cv->set_value(to.value);
        cv->set_scriptpubkey(to.script);
    }
}

}  // namespace

void SetNU5ActivationHeight(uint64_t height) {
    g_nu5_height.store(height, std::memory_order_relaxed);
}

rpc::CompactBlock ParseBlockToCompact(
    std::string_view raw, uint64_t height, uint32_t& sapling_tree_size,
    uint32_t& orchard_tree_size,
    const std::vector<std::string>* txids_display) {
    const bool nu5_active =
        height >= g_nu5_height.load(std::memory_order_relaxed);
    ByteString s(raw);

    // ---- block header ----
    int32_t version;
    if (!s.ReadInt32(&version)) Fail("header version");
    std::string hashPrevBlock;
    if (!s.ReadBytes(&hashPrevBlock, 32)) Fail("hashPrevBlock");
    std::string hashMerkleRoot;
    if (!s.ReadBytes(&hashMerkleRoot, 32)) Fail("hashMerkleRoot");
    std::string hashFinalSaplingRoot;
    if (!s.ReadBytes(&hashFinalSaplingRoot, 32)) Fail("hashFinalSaplingRoot");
    uint32_t time;
    if (!s.ReadUint32(&time)) Fail("time");
    if (!s.Skip(4)) Fail("nBits");
    if (!s.Skip(32)) Fail("nonce");
    std::string solution;
    if (!s.ReadCompactLengthPrefixed(&solution)) Fail("equihash solution");

    // The header is raw[0 .. here]; its SHA256d is the block hash (wire order).
    const size_t header_len = raw.size() - s.size();
    const std::string block_hash = Sha256dStr(raw.substr(0, header_len));

    // ---- transactions ----
    uint64_t txCount;
    if (!s.ReadCompactSize(&txCount)) Fail("tx_count");
    if (txids_display && txCount != txids_display->size()) {
        Fail("tx count mismatch between raw block and verbose txids");
    }

    rpc::CompactBlock cb;
    cb.set_height(height);
    cb.set_prevhash(hashPrevBlock);  // wire order, as parsed
    cb.set_hash(block_hash);
    cb.set_time(time);

    uint64_t block_sapling_outputs = 0, block_orchard_actions = 0;
    for (uint64_t i = 0; i < txCount; ++i) {
        const size_t tx_start = raw.size() - s.size();
        Tx tx;
        ParseTransaction(s, tx, nu5_active);
        const size_t tx_end = raw.size() - s.size();

        // txid: SHA256d of the tx bytes (exact for every non-V5 tx). For V5,
        // the caller supplies the verbose txid instead.
        std::string wire_txid =
            txids_display ? DisplayHexToWire((*txids_display)[i])
                          : Sha256dStr(raw.substr(tx_start, tx_end - tx_start));
        ToCompactTx(tx, static_cast<int>(i), wire_txid, cb.add_vtx());

        block_sapling_outputs += tx.outputs.size();
        block_orchard_actions += tx.actions.size();
    }
    if (!s.empty()) Fail("overlong block (trailing data)");

    // Tree sizes are cumulative note-commitment counts: this block's size = the
    // running total through the previous block plus this block's outputs/actions.
    sapling_tree_size += static_cast<uint32_t>(block_sapling_outputs);
    orchard_tree_size += static_cast<uint32_t>(block_orchard_actions);
    auto* meta = cb.mutable_chainmetadata();
    meta->set_saplingcommitmenttreesize(sapling_tree_size);
    meta->set_orchardcommitmenttreesize(orchard_tree_size);
    return cb;
}

}  // namespace lyghtd
