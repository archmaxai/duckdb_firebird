//! C-ABI Firebird client used by the DuckDB Firebird catalog (storage) extension.
//!
//! This crate exposes a small, stable C interface over the pure-Rust Firebird
//! wire protocol (`rsfbclient` with the `pure_rust` feature). The C++ side of the
//! extension uses it to:
//!   * connect to a Firebird server (no native `fbclient` library required),
//!   * introspect tables and their column types for the DuckDB catalog, and
//!   * stream rows for table scans (one cursor == one dedicated connection).
//!
//! Memory / ownership rules (must be honoured by the C++ caller):
//!   * Every pointer returned by a `*_open` / `*_columns` / `*_tables` function
//!     must be released with the matching `*_free` function.
//!   * `char*` values returned via `out_err` must be released with
//!     [`fb_free_string`].
//!   * Pointers returned by the per-cell `text` / `blob` accessors are owned by
//!     the cursor and remain valid only until the next `fb_cursor_next` call or
//!     until the cursor is freed. Copy the bytes immediately.

use std::ffi::{c_char, c_int, CStr, CString};
use std::os::raw::c_void;
use std::ptr;

use chrono::{NaiveDate, Timelike};
use rsfbclient::{builder_pure_rust, charset, Charset, Queryable, Row, SimpleConnection, SqlType};

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

unsafe fn cstr_to_string(ptr: *const c_char) -> String {
    if ptr.is_null() {
        return String::new();
    }
    CStr::from_ptr(ptr).to_string_lossy().into_owned()
}

/// Write `msg` into `*out_err` as a freshly allocated C string, if `out_err` is non-null.
unsafe fn set_err(out_err: *mut *mut c_char, msg: impl Into<String>) {
    if out_err.is_null() {
        return;
    }
    let c = CString::new(msg.into()).unwrap_or_else(|_| CString::new("error").unwrap());
    *out_err = c.into_raw();
}

fn charset_from_name(name: &str) -> Charset {
    match name.to_ascii_uppercase().replace('-', "_").as_str() {
        "" | "UTF8" | "UTF_8" => charset::UTF_8,
        "ISO8859_1" | "ISO_8859_1" | "LATIN1" => charset::ISO_8859_1,
        "WIN1250" | "WIN_1250" => charset::WIN_1250,
        "WIN1251" | "WIN_1251" => charset::WIN_1251,
        "WIN1252" | "WIN_1252" => charset::WIN_1252,
        "ASCII" | "NONE" => charset::ASCII,
        _ => charset::UTF_8,
    }
}

unsafe fn connect(
    host: *const c_char,
    port: u16,
    user: *const c_char,
    pass: *const c_char,
    db: *const c_char,
    charset_name: *const c_char,
) -> Result<SimpleConnection, String> {
    let host = cstr_to_string(host);
    let user = cstr_to_string(user);
    let pass = cstr_to_string(pass);
    let db = cstr_to_string(db);
    let cs = charset_from_name(&cstr_to_string(charset_name));

    let conn = builder_pure_rust()
        .host(host)
        .port(if port == 0 { 3050 } else { port })
        .user(user)
        .pass(pass)
        .db_name(db)
        .charset(cs)
        .connect()
        .map_err(|e| e.to_string())?;
    Ok(conn.into())
}

// ---------------------------------------------------------------------------
// connection (introspection)
// ---------------------------------------------------------------------------

pub struct FbConn {
    conn: SimpleConnection,
}

/// Open a connection used for catalog introspection. Returns null on error.
#[no_mangle]
pub unsafe extern "C" fn fb_connect(
    host: *const c_char,
    port: u16,
    user: *const c_char,
    pass: *const c_char,
    db: *const c_char,
    charset_name: *const c_char,
    out_err: *mut *mut c_char,
) -> *mut FbConn {
    match connect(host, port, user, pass, db, charset_name) {
        Ok(conn) => Box::into_raw(Box::new(FbConn { conn })),
        Err(e) => {
            set_err(out_err, e);
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn fb_disconnect(conn: *mut FbConn) {
    if !conn.is_null() {
        drop(Box::from_raw(conn));
    }
}

// ---------------------------------------------------------------------------
// table list
// ---------------------------------------------------------------------------

pub struct FbStrList {
    items: Vec<CString>,
}

const LIST_TABLES_SQL: &str = "SELECT TRIM(RDB$RELATION_NAME) FROM RDB$RELATIONS \
     WHERE (RDB$SYSTEM_FLAG IS NULL OR RDB$SYSTEM_FLAG = 0) \
     ORDER BY RDB$RELATION_NAME";

#[no_mangle]
pub unsafe extern "C" fn fb_list_tables(
    conn: *mut FbConn,
    out_err: *mut *mut c_char,
) -> *mut FbStrList {
    if conn.is_null() {
        set_err(out_err, "null connection");
        return ptr::null_mut();
    }
    let conn = &mut *conn;
    let rows: Result<Vec<Row>, _> = conn.conn.query(LIST_TABLES_SQL, ());
    match rows {
        Ok(rows) => {
            let mut items = Vec::with_capacity(rows.len());
            for row in rows {
                let name: String = row.get(0).unwrap_or_default();
                let name = name.trim().to_string();
                if !name.is_empty() {
                    items.push(CString::new(name).unwrap_or_default());
                }
            }
            Box::into_raw(Box::new(FbStrList { items }))
        }
        Err(e) => {
            set_err(out_err, e.to_string());
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn fb_strlist_len(list: *const FbStrList) -> c_int {
    if list.is_null() {
        return 0;
    }
    let list = &*list;
    list.items.len() as c_int
}

#[no_mangle]
pub unsafe extern "C" fn fb_strlist_get(list: *const FbStrList, idx: c_int) -> *const c_char {
    if list.is_null() || idx < 0 {
        return ptr::null();
    }
    let list = &*list;
    match list.items.get(idx as usize) {
        Some(s) => s.as_ptr(),
        None => ptr::null(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn fb_strlist_free(list: *mut FbStrList) {
    if !list.is_null() {
        drop(Box::from_raw(list));
    }
}

// ---------------------------------------------------------------------------
// column metadata
// ---------------------------------------------------------------------------

struct FbColumnMeta {
    name: CString,
    /// Owning table name. Only populated by `fb_all_columns` (the bulk,
    /// single-query introspection path); `None` for single-table results.
    table: Option<CString>,
    field_type: i32,
    sub_type: i32,
    scale: i32,
    length: i32,
    precision: i32,
}

pub struct FbColList {
    cols: Vec<FbColumnMeta>,
}

// Returns: field name, field type, sub type, scale, length, char length, precision.
const TABLE_COLUMNS_SQL: &str = "SELECT TRIM(rf.RDB$FIELD_NAME), f.RDB$FIELD_TYPE, \
     COALESCE(f.RDB$FIELD_SUB_TYPE, 0), COALESCE(f.RDB$FIELD_SCALE, 0), \
     COALESCE(f.RDB$FIELD_LENGTH, 0), COALESCE(f.RDB$CHARACTER_LENGTH, 0), \
     COALESCE(f.RDB$FIELD_PRECISION, 0) \
     FROM RDB$RELATION_FIELDS rf \
     JOIN RDB$FIELDS f ON rf.RDB$FIELD_SOURCE = f.RDB$FIELD_NAME \
     WHERE rf.RDB$RELATION_NAME = ? \
     ORDER BY rf.RDB$FIELD_POSITION";

#[no_mangle]
pub unsafe extern "C" fn fb_table_columns(
    conn: *mut FbConn,
    table: *const c_char,
    out_err: *mut *mut c_char,
) -> *mut FbColList {
    if conn.is_null() {
        set_err(out_err, "null connection");
        return ptr::null_mut();
    }
    let conn = &mut *conn;
    let table = cstr_to_string(table);
    let rows: Result<Vec<Row>, _> = conn.conn.query(TABLE_COLUMNS_SQL, (table.clone(),));
    match rows {
        Ok(rows) => {
            let mut cols = Vec::with_capacity(rows.len());
            for row in rows {
                let name: String = row.get(0).unwrap_or_default();
                let field_type: i64 = row.get(1).unwrap_or(0);
                let sub_type: i64 = row.get(2).unwrap_or(0);
                let scale: i64 = row.get(3).unwrap_or(0);
                let length: i64 = row.get(4).unwrap_or(0);
                let char_length: i64 = row.get(5).unwrap_or(0);
                let precision: i64 = row.get(6).unwrap_or(0);
                let eff_length = if char_length > 0 { char_length } else { length };
                cols.push(FbColumnMeta {
                    name: CString::new(name.trim()).unwrap_or_default(),
                    table: None,
                    field_type: field_type as i32,
                    sub_type: sub_type as i32,
                    scale: scale as i32,
                    length: eff_length as i32,
                    precision: precision as i32,
                });
            }
            Box::into_raw(Box::new(FbColList { cols }))
        }
        Err(e) => {
            set_err(out_err, e.to_string());
            ptr::null_mut()
        }
    }
}

// Returns: relation name, field name, field type, sub type, scale, length,
// char length, precision -- for every column of every user table, ordered by
// table then column position. One query instead of one-per-table avoids a
// disastrous N+1 round-trip pattern over high-latency links.
const ALL_COLUMNS_SQL: &str = "SELECT TRIM(rf.RDB$RELATION_NAME), TRIM(rf.RDB$FIELD_NAME), \
     f.RDB$FIELD_TYPE, COALESCE(f.RDB$FIELD_SUB_TYPE, 0), COALESCE(f.RDB$FIELD_SCALE, 0), \
     COALESCE(f.RDB$FIELD_LENGTH, 0), COALESCE(f.RDB$CHARACTER_LENGTH, 0), \
     COALESCE(f.RDB$FIELD_PRECISION, 0) \
     FROM RDB$RELATION_FIELDS rf \
     JOIN RDB$FIELDS f ON rf.RDB$FIELD_SOURCE = f.RDB$FIELD_NAME \
     JOIN RDB$RELATIONS rel ON rel.RDB$RELATION_NAME = rf.RDB$RELATION_NAME \
     WHERE (rel.RDB$SYSTEM_FLAG IS NULL OR rel.RDB$SYSTEM_FLAG = 0) \
     ORDER BY rf.RDB$RELATION_NAME, rf.RDB$FIELD_POSITION";

/// Fetch column metadata for *all* user tables in a single query. Each returned
/// entry carries its owning table name (see [`fb_collist_table`]).
#[no_mangle]
pub unsafe extern "C" fn fb_all_columns(conn: *mut FbConn, out_err: *mut *mut c_char) -> *mut FbColList {
    if conn.is_null() {
        set_err(out_err, "null connection");
        return ptr::null_mut();
    }
    let conn = &mut *conn;
    let rows: Result<Vec<Row>, _> = conn.conn.query(ALL_COLUMNS_SQL, ());
    match rows {
        Ok(rows) => {
            let mut cols = Vec::with_capacity(rows.len());
            for row in rows {
                let table: String = row.get(0).unwrap_or_default();
                let name: String = row.get(1).unwrap_or_default();
                let field_type: i64 = row.get(2).unwrap_or(0);
                let sub_type: i64 = row.get(3).unwrap_or(0);
                let scale: i64 = row.get(4).unwrap_or(0);
                let length: i64 = row.get(5).unwrap_or(0);
                let char_length: i64 = row.get(6).unwrap_or(0);
                let precision: i64 = row.get(7).unwrap_or(0);
                let eff_length = if char_length > 0 { char_length } else { length };
                cols.push(FbColumnMeta {
                    name: CString::new(name.trim()).unwrap_or_default(),
                    table: Some(CString::new(table.trim()).unwrap_or_default()),
                    field_type: field_type as i32,
                    sub_type: sub_type as i32,
                    scale: scale as i32,
                    length: eff_length as i32,
                    precision: precision as i32,
                });
            }
            Box::into_raw(Box::new(FbColList { cols }))
        }
        Err(e) => {
            set_err(out_err, e.to_string());
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn fb_collist_len(list: *const FbColList) -> c_int {
    if list.is_null() {
        return 0;
    }
    let list = &*list;
    list.cols.len() as c_int
}

#[no_mangle]
pub unsafe extern "C" fn fb_collist_name(list: *const FbColList, idx: c_int) -> *const c_char {
    if list.is_null() || idx < 0 {
        return ptr::null();
    }
    let list = &*list;
    match list.cols.get(idx as usize) {
        Some(c) => c.name.as_ptr(),
        None => ptr::null(),
    }
}

/// Owning table name of a column entry, or null for single-table results that
/// did not record it. Only populated by [`fb_all_columns`].
#[no_mangle]
pub unsafe extern "C" fn fb_collist_table(list: *const FbColList, idx: c_int) -> *const c_char {
    if list.is_null() || idx < 0 {
        return ptr::null();
    }
    let list = &*list;
    match list.cols.get(idx as usize).and_then(|c| c.table.as_ref()) {
        Some(t) => t.as_ptr(),
        None => ptr::null(),
    }
}

macro_rules! collist_int_getter {
    ($name:ident, $field:ident) => {
        #[no_mangle]
        pub unsafe extern "C" fn $name(list: *const FbColList, idx: c_int) -> i32 {
            if list.is_null() || idx < 0 {
                return 0;
            }
            let list = &*list;
            match list.cols.get(idx as usize) {
                Some(c) => c.$field,
                None => 0,
            }
        }
    };
}

collist_int_getter!(fb_collist_field_type, field_type);
collist_int_getter!(fb_collist_sub_type, sub_type);
collist_int_getter!(fb_collist_scale, scale);
collist_int_getter!(fb_collist_length, length);
collist_int_getter!(fb_collist_precision, precision);

#[no_mangle]
pub unsafe extern "C" fn fb_collist_free(list: *mut FbColList) {
    if !list.is_null() {
        drop(Box::from_raw(list));
    }
}

// ---------------------------------------------------------------------------
// scan cursor (streaming)
// ---------------------------------------------------------------------------

/// A streaming scan cursor. Owns its own dedicated connection so scans never
/// contend with the introspection connection or with each other.
///
/// Field drop order matters: `iter` borrows from `*conn` (via a lifetime that we
/// transmute to `'static`), so it must be dropped before `conn`. Rust drops
/// struct fields in declaration order, hence `iter` is declared first.
pub struct FbCursor {
    iter: Option<Box<dyn Iterator<Item = Result<Row, rsfbclient::FbError>>>>,
    conn: *mut SimpleConnection,
    current: Option<Row>,
    text_scratch: Vec<u8>,
}

impl Drop for FbCursor {
    fn drop(&mut self) {
        // Drop the iterator (which borrows *conn) before reclaiming the box.
        self.iter = None;
        if !self.conn.is_null() {
            unsafe { drop(Box::from_raw(self.conn)) };
            self.conn = ptr::null_mut();
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn fb_cursor_open(
    host: *const c_char,
    port: u16,
    user: *const c_char,
    pass: *const c_char,
    db: *const c_char,
    charset_name: *const c_char,
    sql: *const c_char,
    out_err: *mut *mut c_char,
) -> *mut FbCursor {
    let conn = match connect(host, port, user, pass, db, charset_name) {
        Ok(c) => c,
        Err(e) => {
            set_err(out_err, e);
            return ptr::null_mut();
        }
    };
    let sql = cstr_to_string(sql);

    let conn_ptr = Box::into_raw(Box::new(conn));
    // SAFETY: conn_ptr is heap-allocated (stable address) and is not moved or
    // touched again until the cursor (and thus the iterator) is dropped. We
    // extend the iterator's borrow to 'static; the Drop impl guarantees the
    // iterator is destroyed before the connection box is reclaimed.
    let iter_res = (*conn_ptr).query_iter::<(), Row>(&sql, ());
    match iter_res {
        Ok(iter) => {
            let iter: Box<dyn Iterator<Item = Result<Row, rsfbclient::FbError>>> =
                std::mem::transmute(iter);
            Box::into_raw(Box::new(FbCursor {
                iter: Some(iter),
                conn: conn_ptr,
                current: None,
                text_scratch: Vec::new(),
            }))
        }
        Err(e) => {
            set_err(out_err, e.to_string());
            drop(Box::from_raw(conn_ptr));
            ptr::null_mut()
        }
    }
}

/// Advance to the next row. Returns 1 if a row is available, 0 at end of data,
/// -1 on error (with `out_err` set).
#[no_mangle]
pub unsafe extern "C" fn fb_cursor_next(cursor: *mut FbCursor, out_err: *mut *mut c_char) -> c_int {
    if cursor.is_null() {
        set_err(out_err, "null cursor");
        return -1;
    }
    let cursor = &mut *cursor;
    let iter = match cursor.iter.as_mut() {
        Some(i) => i,
        None => {
            cursor.current = None;
            return 0;
        }
    };
    match iter.next() {
        Some(Ok(row)) => {
            cursor.current = Some(row);
            1
        }
        Some(Err(e)) => {
            cursor.current = None;
            set_err(out_err, e.to_string());
            -1
        }
        None => {
            cursor.current = None;
            0
        }
    }
}

unsafe fn cell(cursor: *const FbCursor, idx: c_int) -> Option<&'static SqlType> {
    if cursor.is_null() || idx < 0 {
        return None;
    }
    let cursor = &*cursor;
    let row = cursor.current.as_ref()?;
    row.cols.get(idx as usize).map(|c| &c.value)
}

#[no_mangle]
pub unsafe extern "C" fn fb_cell_is_null(cursor: *const FbCursor, idx: c_int) -> c_int {
    match cell(cursor, idx) {
        Some(SqlType::Null) | None => 1,
        Some(_) => 0,
    }
}

#[no_mangle]
pub unsafe extern "C" fn fb_cell_i64(cursor: *const FbCursor, idx: c_int) -> i64 {
    match cell(cursor, idx) {
        Some(SqlType::Integer(i)) => *i,
        Some(SqlType::Floating(f)) => *f as i64,
        Some(SqlType::Boolean(b)) => *b as i64,
        _ => 0,
    }
}

#[no_mangle]
pub unsafe extern "C" fn fb_cell_f64(cursor: *const FbCursor, idx: c_int) -> f64 {
    match cell(cursor, idx) {
        Some(SqlType::Floating(f)) => *f,
        Some(SqlType::Integer(i)) => *i as f64,
        _ => 0.0,
    }
}

#[no_mangle]
pub unsafe extern "C" fn fb_cell_bool(cursor: *const FbCursor, idx: c_int) -> c_int {
    match cell(cursor, idx) {
        Some(SqlType::Boolean(b)) => *b as c_int,
        Some(SqlType::Integer(i)) => (*i != 0) as c_int,
        _ => 0,
    }
}

fn epoch() -> NaiveDate {
    NaiveDate::from_ymd_opt(1970, 1, 1).unwrap()
}

#[no_mangle]
pub unsafe extern "C" fn fb_cell_date_days(cursor: *const FbCursor, idx: c_int) -> i32 {
    match cell(cursor, idx) {
        Some(SqlType::Timestamp(ts)) => (ts.date() - epoch()).num_days() as i32,
        _ => 0,
    }
}

#[no_mangle]
pub unsafe extern "C" fn fb_cell_time_micros(cursor: *const FbCursor, idx: c_int) -> i64 {
    match cell(cursor, idx) {
        Some(SqlType::Timestamp(ts)) => {
            let t = ts.time();
            t.num_seconds_from_midnight() as i64 * 1_000_000 + (t.nanosecond() as i64) / 1000
        }
        _ => 0,
    }
}

#[no_mangle]
pub unsafe extern "C" fn fb_cell_ts_micros(cursor: *const FbCursor, idx: c_int) -> i64 {
    match cell(cursor, idx) {
        Some(SqlType::Timestamp(ts)) => ts.and_utc().timestamp_micros(),
        _ => 0,
    }
}

/// Returns a pointer to the UTF-8 text bytes of the current cell and writes the
/// byte length to `out_len`. The pointer is valid until the next
/// `fb_cursor_next` call or `fb_cursor_free`. Returns null for non-text/null cells.
#[no_mangle]
pub unsafe extern "C" fn fb_cell_text(
    cursor: *mut FbCursor,
    idx: c_int,
    out_len: *mut usize,
) -> *const c_void {
    if cursor.is_null() || idx < 0 {
        if !out_len.is_null() {
            *out_len = 0;
        }
        return ptr::null();
    }
    let cursor = &mut *cursor;
    let value = match cursor.current.as_ref().and_then(|r| r.cols.get(idx as usize)) {
        Some(c) => &c.value,
        None => {
            if !out_len.is_null() {
                *out_len = 0;
            }
            return ptr::null();
        }
    };
    cursor.text_scratch.clear();
    match value {
        SqlType::Text(s) => cursor.text_scratch.extend_from_slice(s.as_bytes()),
        SqlType::Binary(b) => cursor.text_scratch.extend_from_slice(b),
        SqlType::Integer(i) => cursor.text_scratch.extend_from_slice(i.to_string().as_bytes()),
        SqlType::Floating(f) => cursor.text_scratch.extend_from_slice(f.to_string().as_bytes()),
        SqlType::Boolean(b) => cursor.text_scratch.extend_from_slice(b.to_string().as_bytes()),
        SqlType::Timestamp(ts) => {
            cursor.text_scratch.extend_from_slice(ts.to_string().as_bytes())
        }
        SqlType::Null => {
            if !out_len.is_null() {
                *out_len = 0;
            }
            return ptr::null();
        }
    }
    if !out_len.is_null() {
        *out_len = cursor.text_scratch.len();
    }
    cursor.text_scratch.as_ptr() as *const c_void
}

/// Same contract as [`fb_cell_text`] but for binary BLOB columns.
#[no_mangle]
pub unsafe extern "C" fn fb_cell_blob(
    cursor: *mut FbCursor,
    idx: c_int,
    out_len: *mut usize,
) -> *const c_void {
    if cursor.is_null() || idx < 0 {
        if !out_len.is_null() {
            *out_len = 0;
        }
        return ptr::null();
    }
    let cursor = &mut *cursor;
    let value = match cursor.current.as_ref().and_then(|r| r.cols.get(idx as usize)) {
        Some(c) => &c.value,
        None => {
            if !out_len.is_null() {
                *out_len = 0;
            }
            return ptr::null();
        }
    };
    cursor.text_scratch.clear();
    match value {
        SqlType::Binary(b) => cursor.text_scratch.extend_from_slice(b),
        SqlType::Text(s) => cursor.text_scratch.extend_from_slice(s.as_bytes()),
        _ => {
            if !out_len.is_null() {
                *out_len = 0;
            }
            return ptr::null();
        }
    }
    if !out_len.is_null() {
        *out_len = cursor.text_scratch.len();
    }
    cursor.text_scratch.as_ptr() as *const c_void
}

#[no_mangle]
pub unsafe extern "C" fn fb_cursor_free(cursor: *mut FbCursor) {
    if !cursor.is_null() {
        drop(Box::from_raw(cursor));
    }
}

#[no_mangle]
pub unsafe extern "C" fn fb_free_string(s: *mut c_char) {
    if !s.is_null() {
        drop(CString::from_raw(s));
    }
}
