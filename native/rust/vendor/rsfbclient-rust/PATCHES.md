# Local patches to `rsfbclient-rust` 0.26.0

This is a vendored copy of [`rsfbclient-rust`](https://crates.io/crates/rsfbclient-rust)
`0.26.0`, used via `[patch.crates-io]` in `../../Cargo.toml`. The DuckDB Firebird
catalog extension talks to Firebird over real (often high-latency) networks, where
upstream's behaviour is unusable. The changes below are intentionally minimal and
confined to `src/client.rs` and `src/wire.rs`.

## 1. Batched row fetching (the big one)

Upstream requested exactly **one row per `op_fetch`** (`wire::fetch` hard-coded the
message count to `1`, with a literal `TODO` to increase it). Every row therefore
cost a full network round-trip. Over a ~30 ms relay, listing all columns of a
modest database (≈2,500 rows) took **68 s**; after this patch it takes **~0.75 s**.

Changes:
- `wire::fetch(stmt_handle, blr, message_count)` now takes a row count and sends
  it as the op_fetch "message count".
- `FirebirdWireConnection::fetch` fetches a batch (`FETCH_BATCH_SIZE = 200`) and
  buffers the rows on the statement (`StmtHandleData::pending` / `eof`), handing
  them to the caller one at a time. The public `FirebirdClientSqlOps::fetch`
  signature is unchanged, so the rest of `rsfbclient` is unaffected.
- `parse_fetch_batch` parses an entire batch from an in-memory buffer. Because the
  wire protocol does not length-prefix the overall burst and TCP can split it
  arbitrarily, it is called repeatedly on a growing buffer (reading more from the
  socket on demand) until the batch is complete. Parsing is side-effect free
  (blob bodies are only fetched later in `into_column`), so re-parsing is safe.
- The per-connection read buffer was enlarged to 64 KiB so a batch usually
  arrives in a single `read`.
- `execute` / `execute2` reset the per-statement row buffer (a fresh cursor).

## 2. Socket timeouts

Upstream used `TcpStream::connect((host, port))` with no timeout and no read/write
timeouts, so an unreachable host or a stalled relay would block forever.

Changes (`client.rs`):
- `connect_with_timeout` resolves the address and uses `TcpStream::connect_timeout`
  (default 15 s, override with `FIREBIRD_CONNECT_TIMEOUT_SECS`).
- Read/write timeouts are applied to the socket (default 60 s, override with
  `FIREBIRD_SOCKET_TIMEOUT_SECS`; `0` disables).

## Upgrading

If bumping the upstream version, re-apply these diffs (search for
`FETCH_BATCH_SIZE`, `parse_fetch_batch`, `connect_with_timeout`, and the
`pending` / `eof` fields on `StmtHandleData`).
