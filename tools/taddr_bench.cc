// taddr_bench — micro-benchmark lyghtd vs Go lightwalletd on the transparent
// suite. Both servers proxy the SAME ycashd, so the difference is per-server
// overhead (JSON parse/serialize + proxying), not daemon time. Each RPC is
// timed independently against each server over N iterations (after a warm-up).
//
// usage: taddr_bench [--lyghtd 127.0.0.1:19067] [--oracle 127.0.0.1:9067]
//                    [--iters 12] [--address s1..] [--txid <big-endian-hex>]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include "service.grpc.pb.h"

namespace rpc = cash::z::wallet::sdk::rpc;
using clk = std::chrono::steady_clock;

namespace {

double ToMs(clk::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

std::unique_ptr<rpc::CompactTxStreamer::Stub> Stub(const std::string& a) {
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(-1);
    return rpc::CompactTxStreamer::NewStub(
        grpc::CreateCustomChannel(a, grpc::InsecureChannelCredentials(), args));
}

std::string Unhex(const std::string& h) {
    auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    std::string o;
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        o.push_back(static_cast<char>((nib(h[i]) << 4) | nib(h[i + 1])));
    return o;
}
std::string Rev(const std::string& s) { return std::string(s.rbegin(), s.rend()); }

struct Stats {
    double med = -1, min = -1, mean = -1;
    size_t ok = 0;
};
Stats TimeN(int n, const std::function<bool()>& call) {
    std::vector<double> v;
    call();  // warm-up (untimed)
    for (int i = 0; i < n; ++i) {
        auto t = clk::now();
        bool good = call();
        double e = ToMs(clk::now() - t);
        if (good) v.push_back(e);
    }
    Stats s;
    if (!v.empty()) {
        std::sort(v.begin(), v.end());
        s.min = v.front();
        s.med = v[v.size() / 2];
        double sum = 0;
        for (double x : v) sum += x;
        s.mean = sum / v.size();
        s.ok = v.size();
    }
    return s;
}

void Row(const std::string& name, const Stats& L, const Stats& O) {
    std::printf(
        "  %-26s  lyghtd med %7.1f  min %7.1f  |  oracle med %7.1f  min %7.1f  "
        "ms\n",
        name.c_str(), L.med, L.min, O.med, O.min);
}

}  // namespace

int main(int argc, char** argv) {
    std::string laddr = "127.0.0.1:19067", oaddr = "127.0.0.1:9067";
    int iters = 12;
    std::string addr = "s1Z9YqM2h48HUf8kcSHS89q4Z6Bg9xua3kA";
    std::string txid_be =
        "7db8273bb3ebbaddfbe14add36005d57339792a6d45fcb489cbb98cfb38da50b";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nx = [&] { return std::string(argv[++i]); };
        if (a == "--lyghtd") laddr = nx();
        else if (a == "--oracle") oaddr = nx();
        else if (a == "--iters") iters = std::stoi(nx());
        else if (a == "--address") addr = nx();
        else if (a == "--txid") txid_be = nx();
    }
    auto L = Stub(laddr), O = Stub(oaddr);
    std::printf("transparent-suite bench: %d iters, address %s\n\n", iters,
                addr.c_str());

    // GetTaddressBalance
    auto bal = [&](rpc::CompactTxStreamer::Stub* s) {
        rpc::AddressList req; req.add_addresses(addr);
        rpc::Balance resp; grpc::ClientContext c;
        return s->GetTaddressBalance(&c, req, &resp).ok();
    };
    Row("GetTaddressBalance", TimeN(iters, [&] { return bal(L.get()); }),
        TimeN(iters, [&] { return bal(O.get()); }));

    // GetAddressUtxos (cap 5)
    auto utxo = [&](rpc::CompactTxStreamer::Stub* s) {
        rpc::GetAddressUtxosArg req; req.add_addresses(addr);
        req.set_startheight(0); req.set_maxentries(5);
        rpc::GetAddressUtxosReplyList resp; grpc::ClientContext c;
        return s->GetAddressUtxos(&c, req, &resp).ok();
    };
    Row("GetAddressUtxos(cap 5)", TimeN(iters, [&] { return utxo(L.get()); }),
        TimeN(iters, [&] { return utxo(O.get()); }));

    // GetTransaction
    std::string wire = Rev(Unhex(txid_be));
    auto tx = [&](rpc::CompactTxStreamer::Stub* s) {
        rpc::TxFilter req; req.set_hash(wire);
        rpc::RawTransaction resp; grpc::ClientContext c;
        return s->GetTransaction(&c, req, &resp).ok();
    };
    Row("GetTransaction", TimeN(iters, [&] { return tx(L.get()); }),
        TimeN(iters, [&] { return tx(O.get()); }));

    // GetTaddressTxids over a narrow range (drains the stream)
    auto txids = [&](rpc::CompactTxStreamer::Stub* s) {
        rpc::TransparentAddressBlockFilter req; req.set_address(addr);
        req.mutable_range()->mutable_start()->set_height(1197000);
        req.mutable_range()->mutable_end()->set_height(1197200);
        grpc::ClientContext c;
        auto rd = s->GetTaddressTxids(&c, req);
        rpc::RawTransaction t; uint64_t n = 0;
        while (rd->Read(&t)) ++n;
        return rd->Finish().ok() && n > 0;
    };
    Row("GetTaddressTxids(~106 tx)", TimeN(iters, [&] { return txids(L.get()); }),
        TimeN(iters, [&] { return txids(O.get()); }));

    return 0;
}
