#include "tls_creds.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

#include <openssl/base.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace lyghtd {
namespace {

std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string BioToString(BIO* bio) {
    const uint8_t* data = nullptr;
    size_t len = 0;
    BIO_mem_contents(bio, &data, &len);
    return std::string(reinterpret_cast<const char*>(data), len);
}

// An RSA-2048 self-signed certificate, mirroring lightwalletd's GenerateCerts():
// O="Lighwalletd developer" [sic], 365-day validity, localhost SAN, serverAuth.
// Returns {private_key_pem, cert_pem}.
struct PemPair {
    std::string key;
    std::string cert;
};

PemPair GenerateSelfSigned() {
    // RSA-2048 key via the EVP keygen API.
    bssl::UniquePtr<EVP_PKEY_CTX> kctx(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr));
    if (!kctx || EVP_PKEY_keygen_init(kctx.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(kctx.get(), 2048) <= 0) {
        throw std::runtime_error("TLS: RSA keygen init failed");
    }
    EVP_PKEY* raw_pkey = nullptr;
    if (EVP_PKEY_keygen(kctx.get(), &raw_pkey) <= 0) {
        throw std::runtime_error("TLS: RSA keygen failed");
    }
    bssl::UniquePtr<EVP_PKEY> pkey(raw_pkey);

    bssl::UniquePtr<X509> x509(X509_new());
    if (!x509) throw std::runtime_error("TLS: X509_new failed");
    X509_set_version(x509.get(), 2);  // X.509 v3

    // Random 128-bit positive serial.
    uint8_t serial[16];
    RAND_bytes(serial, sizeof serial);
    serial[0] &= 0x7f;
    bssl::UniquePtr<BIGNUM> bn(BN_bin2bn(serial, sizeof serial, nullptr));
    BN_to_ASN1_INTEGER(bn.get(), X509_get_serialNumber(x509.get()));

    X509_gmtime_adj(X509_getm_notBefore(x509.get()), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509.get()), 60L * 60 * 24 * 365);
    X509_set_pubkey(x509.get(), pkey.get());

    X509_NAME* name = X509_get_subject_name(x509.get());
    X509_NAME_add_entry_by_txt(
        name, "O", MBSTRING_ASC,
        reinterpret_cast<const uint8_t*>("Lighwalletd developer"), -1, -1, 0);
    X509_set_issuer_name(x509.get(), name);  // self-signed: issuer == subject

    // Extensions via the typed i2d API (this BoringSSL omits the CONF-string
    // X509V3_EXT_conf_nid). X509_add1_ext_i2d copies the value, so we free ours.
    {  // basicConstraints: critical, CA:FALSE
        BASIC_CONSTRAINTS* bc = BASIC_CONSTRAINTS_new();
        bc->ca = 0;
        X509_add1_ext_i2d(x509.get(), NID_basic_constraints, bc, 1, 0);
        BASIC_CONSTRAINTS_free(bc);
    }
    {  // keyUsage: critical, digitalSignature | keyEncipherment
        ASN1_BIT_STRING* ku = ASN1_BIT_STRING_new();
        ASN1_BIT_STRING_set_bit(ku, 0, 1);  // digitalSignature
        ASN1_BIT_STRING_set_bit(ku, 2, 1);  // keyEncipherment
        X509_add1_ext_i2d(x509.get(), NID_key_usage, ku, 1, 0);
        ASN1_BIT_STRING_free(ku);
    }
    {  // extKeyUsage: serverAuth
        EXTENDED_KEY_USAGE* eku = sk_ASN1_OBJECT_new_null();
        sk_ASN1_OBJECT_push(eku, OBJ_nid2obj(NID_server_auth));  // static obj
        X509_add1_ext_i2d(x509.get(), NID_ext_key_usage, eku, 0, 0);
        sk_ASN1_OBJECT_free(eku);  // frees the stack, not the static objects
    }
    {  // subjectAltName: DNS:localhost
        GENERAL_NAME* gen = GENERAL_NAME_new();
        ASN1_IA5STRING* dns = ASN1_IA5STRING_new();
        ASN1_STRING_set(dns, "localhost", -1);
        GENERAL_NAME_set0_value(gen, GEN_DNS, dns);  // gen owns dns
        GENERAL_NAMES* gens = sk_GENERAL_NAME_new_null();
        sk_GENERAL_NAME_push(gens, gen);  // gens owns gen
        X509_add1_ext_i2d(x509.get(), NID_subject_alt_name, gens, 0, 0);
        sk_GENERAL_NAME_pop_free(gens, GENERAL_NAME_free);
    }

    if (!X509_sign(x509.get(), pkey.get(), EVP_sha256())) {
        throw std::runtime_error("TLS: X509_sign failed");
    }

    bssl::UniquePtr<BIO> cbio(BIO_new(BIO_s_mem()));
    bssl::UniquePtr<BIO> kbio(BIO_new(BIO_s_mem()));
    if (!PEM_write_bio_X509(cbio.get(), x509.get()) ||
        !PEM_write_bio_PrivateKey(kbio.get(), pkey.get(), nullptr, nullptr, 0,
                                  nullptr, nullptr)) {
        throw std::runtime_error("TLS: PEM encode failed");
    }
    return {BioToString(kbio.get()), BioToString(cbio.get())};
}

}  // namespace

std::shared_ptr<grpc::ServerCredentials> BuildServerCredentials(
    bool no_tls, bool gen_cert, const std::string& cert_path,
    const std::string& key_path) {
    if (no_tls) {
        return grpc::InsecureServerCredentials();
    }
    grpc::SslServerCredentialsOptions opts;
    if (gen_cert) {
        PemPair p = GenerateSelfSigned();
        opts.pem_key_cert_pairs.push_back({std::move(p.key), std::move(p.cert)});
    } else {
        opts.pem_key_cert_pairs.push_back({ReadFile(key_path),
                                           ReadFile(cert_path)});
    }
    return grpc::SslServerCredentials(opts);
}

}  // namespace lyghtd
