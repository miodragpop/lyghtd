# lyghtd

A C++ drop-in port of [lightwalletd](https://github.com/zcash/lightwalletd) for
Ycash. Goal: **byte-for-byte drop-in** for the Go server — reads/writes the same
cache files, speaks the same gRPC contract (`cash.z.wallet.sdk.rpc.CompactTxStreamer`),
works against any `ycashd`.

## Build stack
- **CMake + FetchContent**, pinning a single **gRPC** tag (which vendors a
  matching protobuf — one pin covers both), plus **libcurl** (static, no TLS —
  localhost JSON-RPC) and header-only **glaze** for the ycashd client.
- **Static / self-contained** binary (`BUILD_SHARED_LIBS=OFF`), like the Go one
  (only libc/libstdc++/libm/libgcc are dynamic).
- **C++23**, system GCC. `std::shared_mutex` is exactly the cache concurrency
  model (shared = concurrent readers, unique = ingest append).
- `proto/` holds lyghtd's own copies of the fixed wallet-client contract.
- **Server TLS** reuses the BoringSSL that gRPC already vendors (no OpenSSL
  dependency); the ycashd JSON-RPC client stays plaintext (loopback).

## Building
```sh
./build.sh            # Release
./build.sh Debug      # Debug
```
First build clones + statically builds gRPC/protobuf/abseil/curl from source
(slow, hundreds of MB under `build/_deps`); later builds reuse it.

## Running
```sh
# Read-only frontend (serves an existing cache, no daemon):
./build/lyghtd --tls-cert cert.pem --tls-key cert.key [--bind 127.0.0.1:19067] [--data-dir $HOME/.lightwalletd]

# Full drop-in (also ingests from ycashd into the cache):
./build/lyghtd --ingest --tls-cert cert.pem --tls-key cert.key [--conf $HOME/.ycash/ycash.conf] [...]

# Dev shortcuts (insecure — do not use in production):
./build/lyghtd --gen-cert-very-insecure   # serve TLS with an in-memory self-signed cert
./build/lyghtd --no-tls-very-insecure      # serve plaintext, no TLS
```
**TLS, matching lightwalletd.** TLS is required by default: `--tls-cert`/`--tls-key`
(PEM, default `./cert.pem` / `./cert.key`) must exist, or lyghtd exits. The files
may be self-signed or CA-signed — the server loads either. `--gen-cert-very-insecure`
generates an RSA-2048 self-signed cert in memory (localhost), and
`--no-tls-very-insecure` serves plaintext; both are debug-only, like in lightwalletd.

Without `--ingest` lyghtd opens the cache read-only and just serves. With
`--ingest` it opens the cache writable and runs the standard-RPC ingestor over a
single kept-alive connection (fetch full blocks from ycashd → parse to
CompactBlocks → append) while serving concurrently. Historical blocks (more than
100 below the tip — deeper than the Zcash/Ycash max reorg depth) are fetched
`--ingest-batch` at a time in one batched RPC request, via a producer-consumer
pipeline (one thread fetches the next batch while the writer parses+appends the
current one, overlapping ycashd's work with ours); within the top 100 it
switches to single-block fetch with reorg handling. SIGINT/SIGTERM shut it down
cleanly. Default bind `127.0.0.1:19067` avoids the Go oracle on 9067/9068.

## Milestones
- **0 — toolchain proof (done).** FetchContent static gRPC builds & links a
  trivial `CompactTxStreamer` server that binds a port. No cache, no daemon.
- **1 — read-only frontend (done).** Cache reader over the existing
  `~/.lightwalletd/db/main/` files + the four truly cache-only RPCs
  (GetLightdInfo, GetLatestBlock, GetBlock, GetBlockRange). Validated
  byte-for-byte against the Go oracle at `127.0.0.1:9067`:
  GetBlock identical at heights 419201/570000/571100/2900009; GetBlockRange
  571000..571200 identical (201 blocks, 342 shielded txs); height 0 rejected;
  tip block matches oracle at same height (oracle ingestor runs ahead — lyghtd
  is a read-only startup snapshot). GetTreeState/GetLatestTreeState turned out
  to be daemon-backed (ycashd `z_gettreestate`), so they move to M2.
- **2 — ingestor, standard-RPC path (done).** ycashd JSON-RPC client
  (libcurl + glaze); full block/tx parser (`bytestring` + `block_parser`,
  Sprout/Overwinter/Sapling, plus V5/Orchard gated by NU5 activation height —
  absent on Ycash) building CompactBlocks client-side; cache writer
  (`BlockCache::Add/Reorg/Sync`). `--ingest` runs it while serving. Validated
  byte-for-byte vs the Go cache: parser over 16,210 blocks (all eras) =
  0 mismatches; writer re-serialize of 0..600000 = 7.44 GB identical; end-to-end
  ingest 0..5000 from ycashd identical; live `--ingest` daemon identical.
- **3 — capability probe + fast path (done).** The ingestor probes ycashd via
  `getexperimentalfeatures` and picks the fastest available block source,
  logging which:
  - **`getcompactblockrange`** (when ycashd advertises `compactblocks`): ycashd
    builds the CompactBlocks; lyghtd decodes hex→binary in-parse and stores the
    bytes verbatim (`BlockCache::AddRaw`) — no parse, no hashing. Range size is
    `--ingest-batch` (default 2000, the measured sweet spot).
  - **standard `getblock`** (fallback, older daemons): one raw `getblock` per
    block, with the block hash + txids computed via SHA256d and the note-
    commitment tree sizes via running counters (no second verbose call).
  Both paths reuse the producer/consumer pipeline. Full-chain ingest validated
  byte-for-byte vs the Go cache (~2.93 M blocks, 7.7 GB). Throughput on this
  host went 677 → ~1305 (standard, drop-verbose) → ~1788 blk/s (compact),
  ~2.6× the original; lyghtd stays well under one core (ycashd is the wall).
  TLS serving mirrors lightwalletd (see Running).

## Reference oracle
A Go lightwalletd serving the validated cache runs at `127.0.0.1:9067`
(plaintext; it also occupies 9068 on this host). Every milestone is validated
by diffing C++ gRPC output against it. The milestone-0 stub binds
`127.0.0.1:19067` by default to avoid colliding; override with `lyghtd <addr>`.

## License & acknowledgments
lyghtd is released under the [MIT License](LICENSE), Copyright (c) 2026 Ycash
Foundation.

It is a port of [lightwalletd](https://github.com/zcash/lightwalletd)
(Copyright (c) The Zcash developers, MIT) and deliberately matches its on-disk
cache format and gRPC contract. The Protocol Buffers definitions under `proto/`
originate from that project and retain their original copyright and MIT license
headers.

Built on the open-source [gRPC](https://grpc.io) (Apache-2.0), Protocol Buffers
(BSD-3-Clause), [libcurl](https://curl.se) (curl/MIT), and
[glaze](https://github.com/stephenberry/glaze) (MIT).
