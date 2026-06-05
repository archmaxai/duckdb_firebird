//===----------------------------------------------------------------------===//
// firebird_client.h
//
// C ABI exposed by the `firebird_client` Rust static library. Mirrors the
// `#[no_mangle] extern "C"` functions in native/rust/src/lib.rs.
//===----------------------------------------------------------------------===//
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FbConn FbConn;
typedef struct FbStrList FbStrList;
typedef struct FbColList FbColList;
typedef struct FbCursor FbCursor;

// --- connection (introspection) ---
FbConn *fb_connect(const char *host, uint16_t port, const char *user, const char *pass,
                   const char *db, const char *charset_name, char **out_err);
void fb_disconnect(FbConn *conn);

// --- table list ---
FbStrList *fb_list_tables(FbConn *conn, char **out_err);
int fb_strlist_len(const FbStrList *list);
const char *fb_strlist_get(const FbStrList *list, int idx);
void fb_strlist_free(FbStrList *list);

// --- column metadata ---
FbColList *fb_table_columns(FbConn *conn, const char *table, char **out_err);
// Columns for every user table in a single query; each entry carries its
// owning table name (fb_collist_table).
FbColList *fb_all_columns(FbConn *conn, char **out_err);
int fb_collist_len(const FbColList *list);
const char *fb_collist_name(const FbColList *list, int idx);
// Owning table name (only set for fb_all_columns results), else NULL.
const char *fb_collist_table(const FbColList *list, int idx);
int32_t fb_collist_field_type(const FbColList *list, int idx);
int32_t fb_collist_sub_type(const FbColList *list, int idx);
int32_t fb_collist_scale(const FbColList *list, int idx);
int32_t fb_collist_length(const FbColList *list, int idx);
int32_t fb_collist_precision(const FbColList *list, int idx);
void fb_collist_free(FbColList *list);

// --- scan cursor (streaming) ---
FbCursor *fb_cursor_open(const char *host, uint16_t port, const char *user, const char *pass,
                         const char *db, const char *charset_name, const char *sql, char **out_err);
// 1 = row available, 0 = end of data, -1 = error (out_err set).
int fb_cursor_next(FbCursor *cursor, char **out_err);

int fb_cell_is_null(const FbCursor *cursor, int idx);
int64_t fb_cell_i64(const FbCursor *cursor, int idx);
double fb_cell_f64(const FbCursor *cursor, int idx);
int fb_cell_bool(const FbCursor *cursor, int idx);
int32_t fb_cell_date_days(const FbCursor *cursor, int idx);
int64_t fb_cell_time_micros(const FbCursor *cursor, int idx);
int64_t fb_cell_ts_micros(const FbCursor *cursor, int idx);
const void *fb_cell_text(FbCursor *cursor, int idx, size_t *out_len);
const void *fb_cell_blob(FbCursor *cursor, int idx, size_t *out_len);
void fb_cursor_free(FbCursor *cursor);

void fb_free_string(char *s);

#ifdef __cplusplus
}
#endif
