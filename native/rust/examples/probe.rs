//! Standalone connectivity/latency probe for the real Firebird server.
//!
//! Bypasses DuckDB entirely so we can see exactly where time goes:
//!   1. TCP connect + SRP auth + attach
//!   2. simple `SELECT 1 FROM RDB$DATABASE`
//!   3. the table-list query (materialized)
//!   4. the table-list query (streamed via query_iter)
//!
//! Run with the same env vars used for the extension:
//!   FIREBIRD_HOST=... FIREBIRD_PORT=3050 FIREBIRD_USER=... FIREBIRD_PASSWORD=... \
//!   FIREBIRD_DATABASE=... FIREBIRD_CHARSET=UTF8 \
//!   cargo run --release --example probe

use std::env;
use std::time::Instant;

use rsfbclient::{builder_pure_rust, charset, Queryable, Row};

fn getenv(k: &str, default: &str) -> String {
    env::var(k).unwrap_or_else(|_| default.to_string())
}

fn main() {
    let host = getenv("FIREBIRD_HOST", "127.0.0.1");
    let port: u16 = getenv("FIREBIRD_PORT", "3050").parse().unwrap_or(3050);
    let user = getenv("FIREBIRD_USER", "SYSDBA");
    let pass = getenv("FIREBIRD_PASSWORD", "masterkey");
    let db = getenv("FIREBIRD_DATABASE", "");
    let cs = getenv("FIREBIRD_CHARSET", "UTF8");

    eprintln!("probe: host={host} port={port} user={user} db={db} charset={cs}");

    let t = Instant::now();
    let mut conn = builder_pure_rust()
        .host(host)
        .port(port)
        .user(user)
        .pass(pass)
        .db_name(db)
        .charset(if cs.eq_ignore_ascii_case("UTF8") { charset::UTF_8 } else { charset::ISO_8859_1 })
        .connect()
        .expect("connect failed");
    eprintln!("[1] connect+auth+attach: {:?}", t.elapsed());

    let t = Instant::now();
    let _: Vec<Row> = conn.query("SELECT 1 FROM RDB$DATABASE", ()).expect("ping failed");
    eprintln!("[2] SELECT 1 FROM RDB$DATABASE: {:?}", t.elapsed());

    let sql = "SELECT TRIM(RDB$RELATION_NAME) FROM RDB$RELATIONS \
        WHERE (RDB$SYSTEM_FLAG IS NULL OR RDB$SYSTEM_FLAG = 0) \
        ORDER BY RDB$RELATION_NAME";

    let t = Instant::now();
    let rows: Vec<Row> = conn.query(sql, ()).expect("list (materialized) failed");
    eprintln!("[3] list tables (query, materialized): {:?} -> {} rows", t.elapsed(), rows.len());

    let t = Instant::now();
    let mut n = 0usize;
    for r in conn.query_iter::<(), Row>(sql, ()).expect("query_iter failed") {
        r.expect("row error");
        n += 1;
    }
    eprintln!("[4] list tables (query_iter, streamed): {:?} -> {} rows", t.elapsed(), n);

    // [5] N+1 simulation: one column query per table (current extension behavior),
    // measured over the first 10 tables and extrapolated.
    let per_table_sql = "SELECT TRIM(rf.RDB$FIELD_NAME), f.RDB$FIELD_TYPE \
        FROM RDB$RELATION_FIELDS rf \
        JOIN RDB$FIELDS f ON rf.RDB$FIELD_SOURCE = f.RDB$FIELD_NAME \
        WHERE rf.RDB$RELATION_NAME = ? ORDER BY rf.RDB$FIELD_POSITION";
    let names: Vec<String> = rows.iter().take(10).map(|r| r.get(0).unwrap_or_default()).collect();
    let t = Instant::now();
    let mut cols = 0usize;
    for name in &names {
        let r: Vec<Row> = conn.query(per_table_sql, (name.trim().to_string(),)).expect("per-table failed");
        cols += r.len();
    }
    let per10 = t.elapsed();
    eprintln!(
        "[5] per-table columns x{} tables: {:?} ({} cols)  => extrapolated x{} = {:?}",
        names.len(), per10, cols, n,
        per10.checked_mul((n as u32) / (names.len().max(1) as u32)).unwrap_or_default()
    );

    // [6] bulk: ALL columns for ALL tables in a single query (proposed fix).
    let bulk_sql = "SELECT TRIM(rf.RDB$RELATION_NAME), TRIM(rf.RDB$FIELD_NAME), f.RDB$FIELD_TYPE \
        FROM RDB$RELATION_FIELDS rf \
        JOIN RDB$FIELDS f ON rf.RDB$FIELD_SOURCE = f.RDB$FIELD_NAME \
        JOIN RDB$RELATIONS rel ON rel.RDB$RELATION_NAME = rf.RDB$RELATION_NAME \
        WHERE (rel.RDB$SYSTEM_FLAG IS NULL OR rel.RDB$SYSTEM_FLAG = 0) \
        ORDER BY rf.RDB$RELATION_NAME, rf.RDB$FIELD_POSITION";
    let t = Instant::now();
    let all: Vec<Row> = conn.query(bulk_sql, ()).expect("bulk failed");
    eprintln!("[6] bulk all columns (single query): {:?} -> {} cols", t.elapsed(), all.len());

    eprintln!("probe: OK");
}
