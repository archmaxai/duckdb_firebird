//! DuckDB extension that reads Firebird databases directly using the pure-Rust
//! Firebird wire protocol (`rsfbclient`), with no native `fbclient` required.
//!
//! Exposes two table functions:
//!   * `firebird_query(sql, ...)` — run arbitrary Firebird SQL and stream the result.
//!   * `firebird_tables(...)`     — list user tables in the attached database.
//!
//! Connection parameters are resolved, per field, in this order:
//!   1. an explicit named parameter (`host`, `port`, `user`, `password`,
//!      `database`, `charset`)
//!   2. a component parsed from the `dsn` named parameter
//!      (`firebird://user:pass@host:port/database?charset=UTF8`)
//!   3. the corresponding `FIREBIRD_*` environment variable
//!   4. a built-in default.

use std::error::Error;
use std::sync::atomic::{AtomicUsize, Ordering};

use duckdb::core::{DataChunkHandle, FlatVector, Inserter, LogicalTypeHandle, LogicalTypeId};
use duckdb::vtab::{BindInfo, InitInfo, TableFunctionInfo, VTab};
use duckdb::{Connection, Result};

use rsfbclient::prelude::*;
use rsfbclient::{Row, SqlType};

mod dsn;
use dsn::Dsn;

/// The DuckDB logical type a Firebird column is projected to.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum ColType {
    Boolean,
    Int64,
    Double,
    Varchar,
    Blob,
    Timestamp,
}

impl ColType {
    fn logical_type(self) -> LogicalTypeHandle {
        let id = match self {
            ColType::Boolean => LogicalTypeId::Boolean,
            ColType::Int64 => LogicalTypeId::Bigint,
            ColType::Double => LogicalTypeId::Double,
            ColType::Varchar => LogicalTypeId::Varchar,
            ColType::Blob => LogicalTypeId::Blob,
            ColType::Timestamp => LogicalTypeId::Timestamp,
        };
        LogicalTypeHandle::from(id)
    }
}

struct Column {
    #[allow(dead_code)] // retained for debugging / future schema introspection
    name: String,
    dtype: ColType,
}

/// Materialized result set produced during `bind` and streamed out in `func`.
struct FbBindData {
    columns: Vec<Column>,
    rows: Vec<Vec<SqlType>>,
}

struct FbInitData {
    offset: AtomicUsize,
}

/// Resolved connection parameters.
struct ConnCfg {
    host: String,
    port: u16,
    user: String,
    password: String,
    database: String,
}

/// Firebird treats unquoted login names as case-insensitive and stores the SRP
/// verifier under the upper-cased name, so a lowercase `user` would otherwise
/// fail authentication. A name wrapped in double quotes is taken verbatim
/// (case-sensitive), matching Firebird's quoted-identifier rules.
fn normalize_user(user: &str) -> String {
    let t = user.trim();
    if t.len() >= 2 && t.starts_with('"') && t.ends_with('"') {
        t[1..t.len() - 1].to_string()
    } else {
        t.to_uppercase()
    }
}

fn named(bind: &BindInfo, key: &str) -> Option<String> {
    let v = bind.get_named_parameter(key)?;
    if v.is_null() {
        None
    } else {
        Some(v.to_string())
    }
}

fn resolve_conn(bind: &BindInfo) -> std::result::Result<ConnCfg, Box<dyn Error>> {
    let parsed = match named(bind, "dsn") {
        Some(s) if !s.is_empty() => Some(Dsn::parse(&s)?),
        _ => None,
    };
    let p = parsed.as_ref();

    let pick = |key: &str, from_dsn: Option<String>, env_key: &str| -> Option<String> {
        named(bind, key)
            .or(from_dsn)
            .or_else(|| std::env::var(env_key).ok())
    };

    let host = pick("host", p.and_then(|d| d.host.clone()), "FIREBIRD_HOST")
        .unwrap_or_else(|| "localhost".to_string());
    let user = normalize_user(
        &pick("user", p.and_then(|d| d.user.clone()), "FIREBIRD_USER")
            .unwrap_or_else(|| "SYSDBA".to_string()),
    );
    let password = pick(
        "password",
        p.and_then(|d| d.password.clone()),
        "FIREBIRD_PASSWORD",
    )
    .unwrap_or_else(|| "masterkey".to_string());
    let database = pick(
        "database",
        p.and_then(|d| d.database.clone()),
        "FIREBIRD_DATABASE",
    )
    .ok_or("no Firebird database specified (set `database`, `dsn`, or FIREBIRD_DATABASE)")?;

    let port_str = pick("port", p.and_then(|d| d.port.map(|n| n.to_string())), "FIREBIRD_PORT");
    let port: u16 = match port_str {
        Some(s) => s
            .parse()
            .map_err(|_| format!("invalid Firebird port: {s}"))?,
        None => 3050,
    };

    Ok(ConnCfg {
        host,
        port,
        user,
        password,
        database,
    })
}

/// Connect, run `sql`, and materialize all rows.
fn fetch_rows(cfg: &ConnCfg, sql: &str) -> std::result::Result<Vec<Row>, Box<dyn Error>> {
    let mut conn = rsfbclient::builder_pure_rust()
        .host(cfg.host.as_str())
        .port(cfg.port)
        .user(cfg.user.as_str())
        .pass(cfg.password.as_str())
        .db_name(cfg.database.as_str())
        .connect()
        .map_err(|e| format!("Firebird connection failed: {e}"))?;

    let rows: Vec<Row> = conn
        .query(sql, ())
        .map_err(|e| format!("Firebird query failed: {e}"))?;
    Ok(rows)
}

fn coltype_of_value(v: &SqlType) -> Option<ColType> {
    match v {
        SqlType::Boolean(_) => Some(ColType::Boolean),
        SqlType::Integer(_) => Some(ColType::Int64),
        SqlType::Floating(_) => Some(ColType::Double),
        SqlType::Text(_) => Some(ColType::Varchar),
        SqlType::Binary(_) => Some(ColType::Blob),
        SqlType::Timestamp(_) => Some(ColType::Timestamp),
        SqlType::Null => None,
    }
}

/// Fallback type derived from the Firebird XSQLVAR type code, used when a
/// column is entirely NULL so its runtime variant is never observed.
fn coltype_of_raw(raw_type: u32) -> ColType {
    match raw_type & !1 {
        452 | 448 => ColType::Varchar,             // TEXT, VARYING
        500 | 496 | 580 | 530 => ColType::Int64,   // SHORT, LONG, INT64, QUAD
        482 | 480 => ColType::Double,              // FLOAT, DOUBLE
        510 | 570 | 560 => ColType::Timestamp,     // TIMESTAMP, DATE, TIME
        520 => ColType::Blob,                      // BLOB
        32764 => ColType::Boolean,                 // BOOLEAN
        _ => ColType::Varchar,
    }
}

fn sqltype_to_string(v: &SqlType) -> String {
    match v {
        SqlType::Text(s) => s.clone(),
        SqlType::Integer(i) => i.to_string(),
        SqlType::Floating(f) => f.to_string(),
        SqlType::Boolean(b) => b.to_string(),
        SqlType::Timestamp(ts) => ts.to_string(),
        SqlType::Binary(b) => String::from_utf8_lossy(b).into_owned(),
        SqlType::Null => String::new(),
    }
}

/// Shared bind logic for both table functions.
fn build_bind_data(
    bind: &BindInfo,
    sql: String,
) -> std::result::Result<FbBindData, Box<dyn Error>> {
    let cfg = resolve_conn(bind)?;
    let rows = fetch_rows(&cfg, &sql)?;

    let ncols = rows.first().map(|r| r.cols.len()).unwrap_or(0);

    // Empty result set with no column metadata: expose a single placeholder.
    if ncols == 0 {
        bind.add_result_column("column0", ColType::Varchar.logical_type());
        return Ok(FbBindData {
            columns: vec![Column {
                name: "column0".to_string(),
                dtype: ColType::Varchar,
            }],
            rows: Vec::new(),
        });
    }

    let names: Vec<String> = rows[0].cols.iter().map(|c| c.name.clone()).collect();
    let raws: Vec<u32> = rows[0].cols.iter().map(|c| c.raw_type).collect();

    let data: Vec<Vec<SqlType>> = rows
        .into_iter()
        .map(|r| r.cols.into_iter().map(|c| c.value).collect())
        .collect();

    let mut columns = Vec::with_capacity(ncols);
    for c in 0..ncols {
        let dtype = data
            .iter()
            .find_map(|row| coltype_of_value(&row[c]))
            .unwrap_or_else(|| coltype_of_raw(raws[c]));

        let mut name = names[c].trim().to_string();
        if name.is_empty() {
            name = format!("column{c}");
        }
        bind.add_result_column(&name, dtype.logical_type());
        columns.push(Column { name, dtype });
    }

    Ok(FbBindData {
        columns,
        rows: data,
    })
}

fn write_value(vector: &mut FlatVector, i: usize, dtype: ColType, cell: &SqlType) {
    match (dtype, cell) {
        (_, SqlType::Null) => vector.set_null(i),
        (ColType::Boolean, SqlType::Boolean(b)) => unsafe {
            vector.as_mut_slice::<bool>()[i] = *b;
        },
        (ColType::Int64, SqlType::Integer(v)) => unsafe {
            vector.as_mut_slice::<i64>()[i] = *v;
        },
        (ColType::Double, SqlType::Floating(f)) => unsafe {
            vector.as_mut_slice::<f64>()[i] = *f;
        },
        (ColType::Double, SqlType::Integer(v)) => unsafe {
            vector.as_mut_slice::<f64>()[i] = *v as f64;
        },
        (ColType::Timestamp, SqlType::Timestamp(ts)) => unsafe {
            vector.as_mut_slice::<i64>()[i] = ts.and_utc().timestamp_micros();
        },
        (ColType::Blob, SqlType::Binary(b)) => vector.insert(i, b.as_slice()),
        (ColType::Varchar, SqlType::Text(s)) => vector.insert(i, s.as_str()),
        // Type promoted to VARCHAR (e.g. mixed/unsupported): stringify.
        (ColType::Varchar, other) => vector.insert(i, sqltype_to_string(other).as_str()),
        // Defensive: any unexpected mismatch becomes NULL rather than corrupt data.
        _ => vector.set_null(i),
    }
}

fn emit_chunk(bind_data: &FbBindData, init_data: &FbInitData, output: &mut DataChunkHandle) {
    let offset = init_data.offset.load(Ordering::Relaxed);
    let total = bind_data.rows.len();
    if offset >= total {
        output.set_len(0);
        return;
    }

    let capacity = output.flat_vector(0).capacity();
    let n = std::cmp::min(capacity, total - offset);

    for c in 0..bind_data.columns.len() {
        let dtype = bind_data.columns[c].dtype;
        let mut vector = output.flat_vector(c);
        for i in 0..n {
            write_value(&mut vector, i, dtype, &bind_data.rows[offset + i][c]);
        }
    }

    output.set_len(n);
    init_data.offset.store(offset + n, Ordering::Relaxed);
}

fn connection_named_params() -> Vec<(String, LogicalTypeHandle)> {
    ["dsn", "host", "port", "user", "password", "database", "charset"]
        .iter()
        .map(|k| (k.to_string(), LogicalTypeHandle::from(LogicalTypeId::Varchar)))
        .collect()
}

/// `firebird_query(sql, [dsn=>, host=>, ...])`
struct FirebirdQueryVTab;

impl VTab for FirebirdQueryVTab {
    type InitData = FbInitData;
    type BindData = FbBindData;

    fn bind(bind: &BindInfo) -> std::result::Result<Self::BindData, Box<dyn Error>> {
        let sql = bind.get_parameter(0).to_string();
        build_bind_data(bind, sql)
    }

    fn init(_: &InitInfo) -> std::result::Result<Self::InitData, Box<dyn Error>> {
        Ok(FbInitData {
            offset: AtomicUsize::new(0),
        })
    }

    fn func(
        func: &TableFunctionInfo<Self>,
        output: &mut DataChunkHandle,
    ) -> std::result::Result<(), Box<dyn Error>> {
        emit_chunk(func.get_bind_data(), func.get_init_data(), output);
        Ok(())
    }

    fn parameters() -> Option<Vec<LogicalTypeHandle>> {
        Some(vec![LogicalTypeHandle::from(LogicalTypeId::Varchar)])
    }

    fn named_parameters() -> Option<Vec<(String, LogicalTypeHandle)>> {
        Some(connection_named_params())
    }
}

const LIST_TABLES_SQL: &str = "SELECT TRIM(RDB$RELATION_NAME) AS TABLE_NAME \
     FROM RDB$RELATIONS \
     WHERE RDB$VIEW_BLR IS NULL \
       AND (RDB$SYSTEM_FLAG IS NULL OR RDB$SYSTEM_FLAG = 0) \
     ORDER BY RDB$RELATION_NAME";

/// `firebird_tables([dsn=>, host=>, ...])`
struct FirebirdTablesVTab;

impl VTab for FirebirdTablesVTab {
    type InitData = FbInitData;
    type BindData = FbBindData;

    fn bind(bind: &BindInfo) -> std::result::Result<Self::BindData, Box<dyn Error>> {
        build_bind_data(bind, LIST_TABLES_SQL.to_string())
    }

    fn init(_: &InitInfo) -> std::result::Result<Self::InitData, Box<dyn Error>> {
        Ok(FbInitData {
            offset: AtomicUsize::new(0),
        })
    }

    fn func(
        func: &TableFunctionInfo<Self>,
        output: &mut DataChunkHandle,
    ) -> std::result::Result<(), Box<dyn Error>> {
        emit_chunk(func.get_bind_data(), func.get_init_data(), output);
        Ok(())
    }

    fn named_parameters() -> Option<Vec<(String, LogicalTypeHandle)>> {
        Some(connection_named_params())
    }
}

#[duckdb::duckdb_entrypoint_c_api(ext_name = "firebird", min_duckdb_version = "v1.2.0")]
pub fn extension_entrypoint(con: Connection) -> Result<(), Box<dyn Error>> {
    con.register_table_function::<FirebirdQueryVTab>("firebird_query")?;
    con.register_table_function::<FirebirdTablesVTab>("firebird_tables")?;
    Ok(())
}
