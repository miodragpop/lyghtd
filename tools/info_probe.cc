// info_probe — call GetLightdInfo over TLS against a live lyghtd (the exact
// path ywallet's server probe uses) and dump every field + per-call latency.
// Loops a few times to surface intermittency.
//
// usage: info_probe [addr] [server-cert.pem] [override-name]
//        defaults: localhost:19067  /tmp/lyghtd_server.pem  localhost
//
// The cert is used as the SSL trust root (self-signed gen-cert), and the
// authority is overridden to match the cert SAN (DNS:localhost).

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <grpcpp/grpcpp.h>
#include "service.grpc.pb.h"

namespace rpc = cash::z::wallet::sdk::rpc;

int main(int argc, char** argv) {
    std::string addr = argc > 1 ? argv[1] : "localhost:19067";
    std::string certp = argc > 2 ? argv[2] : "/tmp/lyghtd_server.pem";
    std::string name = argc > 3 ? argv[3] : "localhost";

    std::ifstream f(certp);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string root = ss.str();

    grpc::SslCredentialsOptions opts;
    opts.pem_root_certs = root;
    auto creds = grpc::SslCredentials(opts);

    grpc::ChannelArguments args;
    args.SetSslTargetNameOverride(name);
    args.SetMaxReceiveMessageSize(-1);
    auto chan = grpc::CreateCustomChannel(addr, creds, args);
    auto stub = rpc::CompactTxStreamer::NewStub(chan);

    for (int i = 0; i < 5; ++i) {
        rpc::Empty req;
        rpc::LightdInfo info;
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(5));
        auto t0 = std::chrono::steady_clock::now();
        auto s = stub->GetLightdInfo(&ctx, req, &info);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        std::cout << "--- call " << i << "  [" << ms << "ms]  "
                  << (s.ok() ? "OK" : ("ERR " + std::to_string(s.error_code()) +
                                       " " + s.error_message()))
                  << " ---\n";
        if (s.ok()) {
            std::cout << "  version             = " << info.version() << "\n"
                      << "  vendor              = " << info.vendor() << "\n"
                      << "  taddrSupport        = " << info.taddrsupport() << "\n"
                      << "  chainName           = " << info.chainname() << "\n"
                      << "  saplingActivation   = "
                      << info.saplingactivationheight() << "\n"
                      << "  consensusBranchId   = " << info.consensusbranchid()
                      << "\n"
                      << "  blockHeight         = " << info.blockheight() << "\n"
                      << "  estimatedHeight     = " << info.estimatedheight()
                      << "\n"
                      << "  zcashdBuild         = '" << info.zcashdbuild() << "'\n"
                      << "  zcashdSubversion    = '" << info.zcashdsubversion()
                      << "'\n";
        }
    }
    return 0;
}
