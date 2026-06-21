// grpc_diff — serving-correctness regression check. Issues the same requests to
// a lyghtd instance and the Go reference oracle, and diffs the gRPC responses
// byte-for-byte (via serialized proto). Confirms the served output still matches
// upstream.
//
// usage: grpc_diff [lyghtd_addr] [oracle_addr]
//        (defaults 127.0.0.1:19067 and 127.0.0.1:9067)
//
// Note: GetLatestBlock height may differ if the oracle's ingestor is ahead of
// lyghtd's snapshot; the tool checks lyghtd's tip block against the oracle's
// block AT THAT SAME HEIGHT instead, which must match.

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include "service.grpc.pb.h"

namespace rpc = cash::z::wallet::sdk::rpc;

namespace {

int g_fail = 0;

std::unique_ptr<rpc::CompactTxStreamer::Stub> MakeStub(const std::string& a) {
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(-1);
    return rpc::CompactTxStreamer::NewStub(
        grpc::CreateCustomChannel(a, grpc::InsecureChannelCredentials(), args));
}

void Check(const std::string& name, bool ok, const std::string& detail = "") {
    // Time since the previous Check() == the work this check just did, so slow
    // RPCs stand out. std::endl flushes, so progress streams (no buffering).
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last)
                  .count();
    last = now;
    std::cout << (ok ? "  PASS " : "  FAIL ") << "[" << ms << "ms] " << name;
    if (!detail.empty()) std::cout << " — " << detail;
    std::cout << std::endl;
    if (!ok) ++g_fail;
}

template <typename T>
std::string Ser(const T& m) {
    std::string s;
    m.SerializeToString(&s);
    return s;
}

std::string Hex(const std::string& s) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        out.push_back(d[c >> 4]);
        out.push_back(d[c & 0xf]);
    }
    return out;
}

std::string Unhex(const std::string& h) {
    auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    std::string o;
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        o.push_back(static_cast<char>((nib(h[i]) << 4) | nib(h[i + 1])));
    return o;
}

std::string Rev(const std::string& s) {
    return std::string(s.rbegin(), s.rend());
}

}  // namespace

int main(int argc, char** argv) {
    std::string lyghtd_addr = argc > 1 ? argv[1] : "127.0.0.1:19067";
    std::string oracle_addr = argc > 2 ? argv[2] : "127.0.0.1:9067";
    auto L = MakeStub(lyghtd_addr);
    auto O = MakeStub(oracle_addr);

    // --- GetLatestBlock: tip may lag; compare tip block at same height ---
    {
        rpc::ChainSpec req;
        rpc::BlockID lb, ob;
        grpc::ClientContext c1, c2;
        auto sl = L->GetLatestBlock(&c1, req, &lb);
        auto so = O->GetLatestBlock(&c2, req, &ob);
        Check("GetLatestBlock rpc ok", sl.ok() && so.ok(),
              sl.ok() ? so.error_message() : sl.error_message());
        if (sl.ok() && so.ok()) {
            std::cout << "  INFO lyghtd tip=" << lb.height()
                      << " oracle tip=" << ob.height() << "\n";
            rpc::BlockID req2;
            req2.set_height(lb.height());
            rpc::CompactBlock a, b;
            grpc::ClientContext c3, c4;
            auto s3 = L->GetBlock(&c3, req2, &a);
            auto s4 = O->GetBlock(&c4, req2, &b);
            Check("lyghtd tip block byte-identical to oracle at same height",
                  s3.ok() && s4.ok() && Ser(a) == Ser(b) && a.hash() == lb.hash(),
                  "height " + std::to_string(lb.height()));
        }
    }

    // --- GetBlock at several heights: byte-identical CompactBlock ---
    for (uint64_t h : {419201ULL, 570000ULL, 571100ULL, 2900009ULL}) {
        rpc::BlockID req;
        req.set_height(h);
        rpc::CompactBlock lb, ob;
        grpc::ClientContext c1, c2;
        auto sl = L->GetBlock(&c1, req, &lb);
        auto so = O->GetBlock(&c2, req, &ob);
        std::string tag = "GetBlock(" + std::to_string(h) + ")";
        if (!sl.ok() || !so.ok()) {
            Check(tag + " rpc ok", false,
                  "lyghtd:" + sl.error_message() + " oracle:" + so.error_message());
            continue;
        }
        Check(tag + " byte-identical to oracle", Ser(lb) == Ser(ob),
              "lyghtd " + std::to_string(Ser(lb).size()) + "B vs oracle " +
                  std::to_string(Ser(ob).size()) + "B");
    }

    // --- Height 0 must be rejected ---
    {
        rpc::BlockID req;
        rpc::CompactBlock lb;
        grpc::ClientContext c1;
        auto sl = L->GetBlock(&c1, req, &lb);
        Check("GetBlock(0) rejected",
              sl.error_code() == grpc::StatusCode::INVALID_ARGUMENT,
              sl.error_message());
    }

    // --- GetBlockRange: stream and compare each block byte-for-byte ---
    {
        rpc::BlockRange req;
        req.mutable_start()->set_height(571000);
        req.mutable_end()->set_height(571200);
        grpc::ClientContext c1, c2;
        auto lr = L->GetBlockRange(&c1, req);
        auto orr = O->GetBlockRange(&c2, req);
        rpc::CompactBlock lb, ob;
        uint64_t n = 0, shielded = 0;
        bool mismatch = false;
        uint64_t mismatch_h = 0;
        while (true) {
            bool gl = lr->Read(&lb);
            bool go = orr->Read(&ob);
            if (gl != go) { mismatch = true; mismatch_h = lb.height(); break; }
            if (!gl) break;
            if (Ser(lb) != Ser(ob)) { mismatch = true; mismatch_h = lb.height(); break; }
            for (const auto& tx : lb.vtx())
                if (tx.spends_size() || tx.outputs_size() || tx.actions_size())
                    ++shielded;
            ++n;
        }
        auto fl = lr->Finish();
        auto fo = orr->Finish();
        Check("GetBlockRange(571000..571200) stream matches oracle",
              fl.ok() && fo.ok() && !mismatch,
              mismatch ? ("mismatch at height " + std::to_string(mismatch_h))
                       : (std::to_string(n) + " blocks, " +
                          std::to_string(shielded) + " shielded txs"));
    }

    // ====================================================================
    // Transparent-address suite (daemon-backed; both servers proxy the same
    // ycashd, so responses must be byte-identical).
    // ====================================================================
    const std::string kAddr = "s1Z9YqM2h48HUf8kcSHS89q4Z6Bg9xua3kA";
    const std::string kTxidBE =
        "7db8273bb3ebbaddfbe14add36005d57339792a6d45fcb489cbb98cfb38da50b";

    // --- GetTaddressBalance ---
    {
        rpc::AddressList req;
        req.add_addresses(kAddr);
        rpc::Balance a, b;
        grpc::ClientContext c1, c2;
        auto sl = L->GetTaddressBalance(&c1, req, &a);
        auto so = O->GetTaddressBalance(&c2, req, &b);
        Check("GetTaddressBalance matches oracle",
              sl.ok() && so.ok() && a.valuezat() == b.valuezat(),
              sl.ok() && so.ok() ? ("valueZat " + std::to_string(a.valuezat()))
                                 : (sl.error_message() + " / " + so.error_message()));
    }

    // --- GetAddressUtxos (capped) — exercises txid reversal + field mapping ---
    {
        rpc::GetAddressUtxosArg req;
        req.add_addresses(kAddr);
        req.set_startheight(0);
        req.set_maxentries(5);
        rpc::GetAddressUtxosReplyList a, b;
        grpc::ClientContext c1, c2;
        auto sl = L->GetAddressUtxos(&c1, req, &a);
        auto so = O->GetAddressUtxos(&c2, req, &b);
        Check("GetAddressUtxos(maxEntries=5) byte-identical to oracle",
              sl.ok() && so.ok() && Ser(a) == Ser(b),
              sl.ok() && so.ok()
                  ? (std::to_string(a.addressutxos_size()) + " vs " +
                     std::to_string(b.addressutxos_size()) + " utxos")
                  : (sl.error_message() + " / " + so.error_message()));
    }

    // --- GetTransaction (txid is wire/little-endian in the request) ---
    {
        rpc::TxFilter req;
        req.set_hash(Rev(Unhex(kTxidBE)));
        rpc::RawTransaction a, b;
        grpc::ClientContext c1, c2;
        auto sl = L->GetTransaction(&c1, req, &a);
        auto so = O->GetTransaction(&c2, req, &b);
        Check("GetTransaction byte-identical to oracle",
              sl.ok() && so.ok() && Ser(a) == Ser(b),
              sl.ok() && so.ok()
                  ? ("data " + std::to_string(a.data().size()) + "B, height " +
                     std::to_string(a.height()))
                  : (sl.error_message() + " / " + so.error_message()));
    }

    // --- GetTaddressTxids over a narrow range: stream must match ---
    {
        rpc::TransparentAddressBlockFilter req;
        req.set_address(kAddr);
        req.mutable_range()->mutable_start()->set_height(1197000);
        req.mutable_range()->mutable_end()->set_height(1197200);
        grpc::ClientContext c1, c2;
        auto lr = L->GetTaddressTxids(&c1, req);
        auto orr = O->GetTaddressTxids(&c2, req);
        rpc::RawTransaction la, ob;
        uint64_t n = 0;
        bool mismatch = false;
        while (true) {
            bool gl = lr->Read(&la);
            bool go = orr->Read(&ob);
            if (gl != go) { mismatch = true; break; }
            if (!gl) break;
            if (Ser(la) != Ser(ob)) { mismatch = true; break; }
            ++n;
        }
        auto fl = lr->Finish();
        auto fo = orr->Finish();
        Check("GetTaddressTxids(1197000..1197200) stream matches oracle",
              fl.ok() && fo.ok() && !mismatch,
              mismatch ? "stream mismatch"
                       : (std::to_string(n) + " transactions"));
    }

    // ====================================================================
    // Spend path (daemon-backed): z_gettreestate + sendrawtransaction.
    // ====================================================================

    // --- GetTreeState at a fixed height ---
    {
        rpc::BlockID req;
        req.set_height(1500000);
        rpc::TreeState a, b;
        grpc::ClientContext c1, c2;
        auto sl = L->GetTreeState(&c1, req, &a);
        auto so = O->GetTreeState(&c2, req, &b);
        Check("GetTreeState(1500000) byte-identical to oracle",
              sl.ok() && so.ok() && Ser(a) == Ser(b),
              sl.ok() && so.ok()
                  ? ("h" + std::to_string(a.height()) + ", saplingTree " +
                     std::to_string(a.saplingtree().size()) + "B, orchard " +
                     std::to_string(a.orchardtree().size()) + "B")
                  : (sl.error_message() + " / " + so.error_message()));
    }

    // --- GetLatestTreeState (tip can advance between the two calls) ---
    {
        rpc::Empty req;
        rpc::TreeState a, b;
        grpc::ClientContext c1, c2;
        auto sl = L->GetLatestTreeState(&c1, req, &a);
        auto so = O->GetLatestTreeState(&c2, req, &b);
        // If the tip raced, heights differ — still a pass; else must be identical.
        bool ok = sl.ok() && so.ok() &&
                  (a.height() != b.height() || Ser(a) == Ser(b));
        Check("GetLatestTreeState matches oracle (or tip raced)", ok,
              sl.ok() && so.ok()
                  ? ("lyghtd h" + std::to_string(a.height()) + " / oracle h" +
                     std::to_string(b.height()))
                  : (sl.error_message() + " / " + so.error_message()));
    }

    // --- SendTransaction: resubmit an already-confirmed tx; ycashd rejects it
    //     identically on both servers (no real broadcast happens). ---
    {
        rpc::TxFilter tf;
        tf.set_hash(Rev(Unhex(kTxidBE)));
        rpc::RawTransaction tx;
        grpc::ClientContext c0;
        auto sg = L->GetTransaction(&c0, tf, &tx);
        rpc::RawTransaction send_req;
        send_req.set_data(tx.data());
        rpc::SendResponse a, b;
        grpc::ClientContext c1, c2;
        auto sl = L->SendTransaction(&c1, send_req, &a);
        auto so = O->SendTransaction(&c2, send_req, &b);
        Check("SendTransaction(confirmed tx) rejects identically to oracle",
              sg.ok() && sl.ok() && so.ok() && Ser(a) == Ser(b),
              sl.ok() && so.ok()
                  ? ("code " + std::to_string(a.errorcode()) + " msg '" +
                     a.errormessage() + "'")
                  : (sl.error_message() + " / " + so.error_message()));
    }

    // ====================================================================
    // Mempool (daemon-backed). Both proxy the same ycashd mempool.
    // ====================================================================

    // --- GetMempoolTx: compact-tx stream, byte-for-byte ---
    {
        rpc::GetMempoolTxRequest req;  // no excludes, default (shielded) pools
        grpc::ClientContext c1, c2;
        auto lr = L->GetMempoolTx(&c1, req);
        auto orr = O->GetMempoolTx(&c2, req);
        rpc::CompactTx la, ob;
        uint64_t n = 0;
        bool mismatch = false;
        while (true) {
            bool gl = lr->Read(&la), go = orr->Read(&ob);
            if (gl != go) { mismatch = true; break; }
            if (!gl) break;
            if (Ser(la) != Ser(ob)) { mismatch = true; break; }
            ++n;
        }
        auto fl = lr->Finish(), fo = orr->Finish();
        Check("GetMempoolTx stream matches oracle",
              fl.ok() && fo.ok() && !mismatch,
              mismatch ? "stream mismatch"
                       : (std::to_string(n) + " shielded mempool txs"));
    }

    // --- GetMempoolStream: warm-up (resets shared state to the current block),
    //     then a deadline-bounded real stream; compare the tx set. ---
    {
        auto drain = [](rpc::CompactTxStreamer::Stub* s,
                        std::vector<std::string>& out) {
            {  // warm-up: sync the monitor's shared state to the current block
                grpc::ClientContext wc;
                wc.set_deadline(std::chrono::system_clock::now() +
                                std::chrono::milliseconds(700));
                auto rd = s->GetMempoolStream(&wc, rpc::Empty());
                rpc::RawTransaction t;
                while (rd->Read(&t)) {
                }
                rd->Finish();
            }
            grpc::ClientContext c;
            c.set_deadline(std::chrono::system_clock::now() +
                           std::chrono::milliseconds(2200));
            auto rd = s->GetMempoolStream(&c, rpc::Empty());
            rpc::RawTransaction t;
            while (rd->Read(&t)) out.push_back(Ser(t));
            rd->Finish();
        };
        std::vector<std::string> lv, ov;
        drain(L.get(), lv);
        drain(O.get(), ov);
        std::sort(lv.begin(), lv.end());
        std::sort(ov.begin(), ov.end());
        Check("GetMempoolStream matches oracle (warm-up + 2s window)", lv == ov,
              "lyghtd " + std::to_string(lv.size()) + " / oracle " +
                  std::to_string(ov.size()) + " txs");
    }

    std::cout << (g_fail == 0 ? "\nALL CHECKS PASSED\n"
                              : "\n" + std::to_string(g_fail) + " CHECK(S) FAILED\n");
    return g_fail == 0 ? 0 : 1;
}
