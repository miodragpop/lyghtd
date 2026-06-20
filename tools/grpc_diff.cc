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

#include <iostream>
#include <memory>
#include <string>

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
    std::cout << (ok ? "  PASS " : "  FAIL ") << name;
    if (!detail.empty()) std::cout << " — " << detail;
    std::cout << "\n";
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

    std::cout << (g_fail == 0 ? "\nALL CHECKS PASSED\n"
                              : "\n" + std::to_string(g_fail) + " CHECK(S) FAILED\n");
    return g_fail == 0 ? 0 : 1;
}
