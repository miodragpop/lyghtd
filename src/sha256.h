#pragma once

// Minimal self-contained SHA-256 (and SHA-256d = double SHA-256). Used to
// compute block hashes from the header and transaction ids from tx bytes, so
// the ingestor can drop the verbose getblock call (txid = SHA256d(tx bytes) for
// all non-V5 transactions, which is every transaction on Ycash).

#include <cstddef>
#include <cstdint>

namespace lyghtd {

// 32-byte SHA-256 of `data`.
void SHA256(const void* data, size_t len, uint8_t out[32]);

// SHA-256d: SHA256(SHA256(data)). This is the Zcash/Bitcoin block- and
// tx-hash function. Output is in internal/wire (little-endian) byte order —
// exactly what CompactBlock stores; the display form is the byte-reverse.
void SHA256d(const void* data, size_t len, uint8_t out[32]);

}  // namespace lyghtd
