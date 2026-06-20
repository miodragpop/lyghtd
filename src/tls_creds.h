#pragma once

// Server TLS credentials, replicating Go lightwalletd's three modes
// (cmd/root.go + common/generatecerts.go) so lyghtd is a drop-in there too:
//
//   no_tls   -> insecure plaintext (lightwalletd --no-tls-very-insecure)
//   gen_cert -> RSA-2048 self-signed cert generated in memory, localhost SAN
//               (lightwalletd --gen-cert-very-insecure / GenerateCerts())
//   else     -> load PEM cert+key from cert_path/key_path (the default mode;
//               works for both self-signed and CA-signed files — the server
//               doesn't distinguish). lightwalletd --tls-cert/--tls-key.
//
// In-memory generation uses BoringSSL (vendored by gRPC, already linked).

#include <memory>
#include <string>

#include <grpcpp/security/server_credentials.h>

namespace lyghtd {

std::shared_ptr<grpc::ServerCredentials> BuildServerCredentials(
    bool no_tls, bool gen_cert, const std::string& cert_path,
    const std::string& key_path);

}  // namespace lyghtd
