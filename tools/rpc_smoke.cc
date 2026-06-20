// rpc_smoke — exercise the ycashd RPC client against a live daemon and print
// what it returns. A quick "is the client + daemon healthy" check.
//
// usage: rpc_smoke [conf-path]   (default $HOME/.ycash/ycash.conf)

#include <cstdlib>
#include <iostream>

#include "rpc_client.h"

int main(int argc, char** argv) {
    std::string conf = argc > 1 ? argv[1]
                                 : std::string(std::getenv("HOME")) +
                                       "/.ycash/ycash.conf";
    try {
        auto rpc = lyghtd::RpcClient::FromConf(conf);
        std::cout << "connected to " << rpc.host() << ":" << rpc.port() << "\n";

        auto ci = rpc.GetBlockChainInfo();
        std::cout << "getblockchaininfo: chain=" << ci.chain
                  << " blocks=" << ci.blocks
                  << " chaintip=" << ci.consensus_chaintip
                  << " saplingActivation=" << ci.sapling_activation_height
                  << " estimatedHeight=" << ci.estimated_height << "\n";

        auto info = rpc.GetInfo();
        std::cout << "getinfo: build=" << info.build
                  << " subversion=" << info.subversion << "\n";

        std::string best = rpc.GetBestBlockHash();
        std::cout << "getbestblockhash: " << best << "\n";

        auto vb = rpc.GetBlockVerbose(419201);
        std::cout << "getblock 419201 verbose: hash=" << vb.hash
                  << " ntx=" << vb.tx.size()
                  << " saplingTreeSize=" << vb.sapling_tree_size
                  << " orchardTreeSize=" << vb.orchard_tree_size << "\n";

        std::string raw = rpc.GetBlockRawHex(vb.hash);
        std::cout << "getblock raw: " << raw.size() << " hex chars ("
                  << raw.size() / 2 << " bytes)\n";
        std::cout << "OK\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
