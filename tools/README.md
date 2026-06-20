# lyghtd dev / regression-testing tools

Validation and benchmark harnesses for checking lyghtd against the Go
lightwalletd (the cache it wrote + a running oracle) and against ycashd. They
are **not built by default** — they link `lyghtd_core`, so they run exactly the
same compiled code as the server.

## Building
```sh
cd build
cmake -DLYGHTD_BUILD_TOOLS=ON ..
make -j"$(nproc)"
# binaries land in build/
```

## Tools

| Tool | Checks | Needs |
|------|--------|-------|
| `rpc_smoke [conf]` | ycashd RPC client health (chain info, getinfo, getblock verbose+raw) | ycashd |
| `parse_validate [--ranges a-b,...] [--step N]` | **parser**: fetch full blocks from ycashd, parse to CompactBlocks, diff byte-for-byte vs the Go cache | ycashd + Go cache |
| `cache_validate --mode {reserialize\|ingest} --target N` | **writer / ingest**: write a fresh cache prefix and diff byte-for-byte vs the Go cache files. `reserialize` re-writes Go's CompactBlocks (fast, all block types); `ingest` runs the real RPC→parse→write pipeline | Go cache (+ ycashd for `ingest`) |
| `bench_client [addr]` | **serving throughput**: stream GetBlockRange(1..tip), report blk/s + bytes | a running lyghtd |
| `grpc_diff [lyghtd] [oracle]` | **serving correctness**: diff gRPC responses (GetBlock/GetBlockRange/GetLatestBlock/height-0) vs the Go oracle | a running lyghtd + Go oracle |
| `ingest_profile [--start H] [--count N] [--threads M]` | **ingest profiling**: per-stage timing (verbose RPC / raw RPC / hex / parse, with curl transfer time split out); `--threads M` measures ycashd's concurrent getblock ceiling | ycashd + Go cache |

## Typical regression run
```sh
# correctness of the parser against the validated Go cache
./build/parse_validate

# writer format over all block types (fast, ~90% of cache bytes by height 600k)
./build/cache_validate --mode reserialize --target 600000

# end-to-end ingest of a fresh prefix
./build/cache_validate --mode ingest --target 5000

# serving correctness + throughput (lyghtd on :19067, Go oracle on :9067)
./build/grpc_diff
./build/bench_client 127.0.0.1:19067
```

## Notes
- `parse_validate` / `cache_validate` default `--data-dir $HOME/.lightwalletd`,
  `--chain main`, `--conf $HOME/.ycash/ycash.conf`. They open the Go cache
  **read-only**; `cache_validate` writes only to its `--out` temp dir
  (default `/tmp/lyghtd_cache_validate`).
- Do **not** run `cache_validate --mode ingest` with `--out` pointed at a cache
  another process (the Go oracle) is also writing.
- All tools `return 0` on success, non-zero on failure — usable in CI.
