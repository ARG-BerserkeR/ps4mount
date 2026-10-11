/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * app.db / appinfo.db rows for titles registered without installing. */
/* app.db / appinfo.db rows for a PS4 title registered without installing.
 * Column sets and values mirror a real fpkg install on FW 13.20, with the
 * offline concept id form ("cid:local:<TID>") the system uses itself. */
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sqlite3.h"
#include "build.h"

#ifndef APP_DB
#define APP_DB "/system_data/priv/mms/app.db"
#endif
#ifndef APPINFO_DB
#define APPINFO_DB "/system_data/priv/mms/appinfo.db"
#endif
#define MAX_USERS 16
#define MAX_KV 256

typedef struct {
    char key[64];
    int is_int;
    long long i;
    char s[512];
} kv_t;

typedef struct {
    int n;
    kv_t v[MAX_KV];
} kvs_t;

static void kv_int(kvs_t *k, const char *key, long long v) {
    if (k->n >= MAX_KV)
        return;
    kv_t *e = &k->v[k->n++];
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->is_int = 1;
    e->i = v;
}

static void kv_str(kvs_t *k, const char *key, const char *v) {
    if (k->n >= MAX_KV)
        return;
    kv_t *e = &k->v[k->n++];
    snprintf(e->key, sizeof(e->key), "%s", key);
    e->is_int = 0;
    snprintf(e->s, sizeof(e->s), "%s", v);
}

static int kv_has(const kvs_t *k, const char *key) {
    for (int i = 0; i < k->n; i++)
        if (!strcmp(k->v[i].key, key))
            return 1;
    return 0;
}

static int kv_cmp(const void *a, const void *b) { return strcmp(((const kv_t *)a)->key, ((const kv_t *)b)->key); }

static void json_str(buf_t *b, const char *s) {
    buf_put(b, "\"", 1);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\' || *s == '/')
            buf_put(b, "\\", 1);
        buf_put(b, s, 1);
    }
    buf_put(b, "\"", 1);
}

static void kv_json_object(buf_t *b, kvs_t *k) {
    qsort(k->v, (size_t)k->n, sizeof(kv_t), kv_cmp);
    buf_put(b, "{", 1);
    for (int i = 0; i < k->n; i++) {
        if (i)
            buf_put(b, ",", 1);
        json_str(b, k->v[i].key);
        buf_put(b, ":", 1);
        if (k->v[i].is_int)
            buf_printf(b, "%lld", k->v[i].i);
        else
            json_str(b, k->v[i].s);
    }
    buf_put(b, "}", 1);
}

static void kv_field_list(buf_t *b, kvs_t *k) {
    qsort(k->v, (size_t)k->n, sizeof(kv_t), kv_cmp);
    buf_put(b, "{\"field_list\":[", 15);
    for (int i = 0; i < k->n; i++) {
        buf_printf(b, "%s{\"data\":", i ? "," : "");
        if (k->v[i].is_int)
            buf_printf(b, "%lld", k->v[i].i);
        else
            json_str(b, k->v[i].s);
        buf_put(b, ",\"key\":", 7);
        json_str(b, k->v[i].key);
        buf_printf(b, ",\"size\":%zu,\"type\":%d}", k->v[i].is_int ? (size_t)8 : strlen(k->v[i].s),
                   k->v[i].is_int ? 0 : 2);
    }
    buf_printf(b, "],\"mimeType\":0,\"mimeTypeFormatVersion\":0,\"parserId\":0,\"promoterFormatVersion\":0}");
}

/* Column/value list for one INSERT OR REPLACE. */
typedef struct {
    int n;
    const char *col[64];
    kv_t val[64];
} row_t;

static void ci(row_t *r, const char *c, long long v) {
    r->col[r->n] = c;
    r->val[r->n].is_int = 1;
    r->val[r->n].i = v;
    r->n++;
}

static void cs(row_t *r, const char *c, const char *v) {
    r->col[r->n] = c;
    r->val[r->n].is_int = 0;
    snprintf(r->val[r->n].s, sizeof(r->val[r->n].s), "%s", v ? v : "");
    r->n++;
}

static int insert_row(sqlite3 *db, const char *table, const row_t *r, const char *long_col, const char *long_val) {
    buf_t sql = {0};
    buf_printf(&sql, "INSERT OR REPLACE INTO \"%s\" (", table);
    for (int i = 0; i < r->n; i++)
        buf_printf(&sql, "%s\"%s\"", i ? "," : "", r->col[i]);
    if (long_col)
        buf_printf(&sql, ",\"%s\"", long_col);
    buf_printf(&sql, ") VALUES (");
    for (int i = 0; i < r->n + (long_col ? 1 : 0); i++)
        buf_printf(&sql, "%s?", i ? "," : "");
    buf_printf(&sql, ")");
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, (const char *)sql.data, -1, &st, NULL);
    buf_free(&sql);
    if (rc != SQLITE_OK) {
        printf("[db] prepare %s: %s\n", table, sqlite3_errmsg(db));
        return -1;
    }
    for (int i = 0; i < r->n; i++) {
        if (r->val[i].is_int)
            sqlite3_bind_int64(st, i + 1, r->val[i].i);
        else
            sqlite3_bind_text(st, i + 1, r->val[i].s, -1, SQLITE_TRANSIENT);
    }
    if (long_col)
        sqlite3_bind_text(st, r->n + 1, long_val, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        printf("[db] insert %s: %s\n", table, sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

static int open_db(const char *path, sqlite3 **db) {
    if (sqlite3_open_v2(path, db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        printf("[db] open %s: %s\n", path, *db ? sqlite3_errmsg(*db) : "?");
        sqlite3_close(*db);
        return -1;
    }
    sqlite3_busy_timeout(*db, 10000);
    return 0;
}

static long long version_next(sqlite3 *db, const char *category) {
    long long v = 0;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT status FROM tbl_version WHERE category=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, category, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW)
            v = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    v++;
    if (sqlite3_prepare_v2(db, "UPDATE tbl_version SET status=? WHERE category=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, v);
        sqlite3_bind_text(st, 2, category, -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    return v;
}

static int user_tables(sqlite3 *db, unsigned *uids) {
    int n = 0;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_iconinfo_%'", -1,
                           &st, NULL) != SQLITE_OK)
        return 0;
    while (sqlite3_step(st) == SQLITE_ROW && n < MAX_USERS)
        uids[n++] = (unsigned)strtoul((const char *)sqlite3_column_text(st, 0) + 13, NULL, 10);
    sqlite3_finalize(st);
    return n;
}

int db_is_registered(const char *title_id) {
    sqlite3 *db;
    if (sqlite3_open_v2(APP_DB, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return 0;
    }
    sqlite3_busy_timeout(db, 5000);
    sqlite3_stmt *st;
    int found = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM tbl_contentinfo WHERE titleId=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, title_id, -1, SQLITE_STATIC);
        found = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return found;
}

static void now_str(char *out, size_t n) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d.%03ld", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
             tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
}

int db_register(const db_title_t *t) {
    const char *tid = t->pkg->title_id, *cid = t->pkg->content_id;
    char now[32], concept[64], meta[64], deeplink[64], hub[64], icon[128] = "", pic[128] = "", key[64];
    now_str(now, sizeof(now));
    snprintf(concept, sizeof(concept), "cid:local:%s", tid);
    snprintf(meta, sizeof(meta), "/user/appmeta/%s", tid);
    snprintf(deeplink, sizeof(deeplink), "psgm:play?id=%s", tid);
    snprintf(hub, sizeof(hub), "pshome:gamehub?titleId=%s", tid);
    if (t->icon_ts)
        snprintf(icon, sizeof(icon), "%s/icon0.png?ts=%lld", meta, (long long)t->icon_ts);
    if (t->pic_ts)
        snprintf(pic, sizeof(pic), "%s/pic0.dds?ts=%lld", meta, (long long)t->pic_ts);
    const sfo_item_t *it;
    long long parental = (it = sfo_get(t->sfo, "PARENTAL_LEVEL")) ? it->ival : 0;
    long long attribute = (it = sfo_get(t->sfo, "ATTRIBUTE")) ? it->ival : 0;
    long long app_type = (it = sfo_get(t->sfo, "APP_TYPE")) ? it->ival : 1;

    sqlite3 *adb = NULL, *idb = NULL;
    if (open_db(APP_DB, &adb) || open_db(APPINFO_DB, &idb)) {
        sqlite3_close(adb);
        return -1;
    }
    unsigned uids[MAX_USERS];
    int nusers = user_tables(adb, uids);
    int rc = -1;
    sqlite3_exec(adb, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    sqlite3_exec(idb, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    long long icon_access = version_next(adb, "access_index"); /* per-user icon tables */
    long long app_sync = version_next(idb, "appinfo_sync_index");
    long long concept_sync = version_next(idb, "conceptinfo_sync_index");
    long long access = version_next(idb, "access_index"); /* contentinfo / appinfo */

    /* appinfo key/values: param.sfo + fixed system keys */
    static kvs_t kv;
    memset(&kv, 0, sizeof(kv));
    for (int i = 0; i < t->sfo->count; i++) {
        const sfo_item_t *s = &t->sfo->items[i];
        if (!strcmp(s->key, "DEV_FLAG") || !strcmp(s->key, "PUBTOOLINFO") || !strcmp(s->key, "PUBTOOLVER") ||
            !strcmp(s->key, "PUBTOOLMINVER"))
            continue;
        if (!strcmp(s->key, "CATEGORY"))
            kv_str(&kv, s->key, "gd"); /* a patch sfo says "gp"; the title stays a game */
        else if (s->is_int)
            kv_int(&kv, s->key, s->ival);
        else if (!s->sval[0])
            kv_int(&kv, s->key, 0);
        else
            kv_str(&kv, s->key, s->sval);
    }
    static const char *zero_keys[] = {"ATTRIBUTE_INTERNAL", "DISP_LOCATION_1", "DISP_LOCATION_2", "PT_PARAM",
                                      "PUBTOOL_VERSION", "USER_DEFINED_PARAM_1", "USER_DEFINED_PARAM_2",
                                      "USER_DEFINED_PARAM_3", "USER_DEFINED_PARAM_4", NULL};
    for (int i = 0; zero_keys[i]; i++)
        if (!kv_has(&kv, zero_keys[i]))
            kv_int(&kv, zero_keys[i], 0);
    kv_int(&kv, "CATEGORY_TYPE", 61440);
    kv_int(&kv, "DISPLAYLOCATION", 138);
    kv_str(&kv, "DEEPLINK_URI", deeplink);
    kv_str(&kv, "HUBAPP_URI", hub);
    kv_str(&kv, "METADATA_ID", "prior:internal:0");
    for (int u = 0; u < nusers; u++) {
        snprintf(key, sizeof(key), "#_access_index_%08x", uids[u]);
        kv_int(&kv, key, access);
        snprintf(key, sizeof(key), "#_last_access_time_%08x", uids[u]);
        kv_str(&kv, key, now);
    }
    static const char *zero_internal[] = {"#_contents_status", "#exit_type", "_app_format_type", "_contents_ext_type",
                                          "_contents_location", "_current_slot", "_disable_live_detail",
                                          "_external_hdd_app_status", "_hdd_location", "_install_status",
                                          "_large_pkg_type", "_m2_device_id", "_not_install_sub_status",
                                          "_path_changeinfo_info", "_path_info", "_path_info_2",
                                          "_path_promotion0_info", "_size_other_hdd", "_view_category", NULL};
    for (int i = 0; zero_internal[i]; i++)
        kv_int(&kv, zero_internal[i], 0);
    kv_str(&kv, "#_ctime", now);
    kv_str(&kv, "#_install_time", now);
    kv_str(&kv, "#_mtime", now);
    kv_str(&kv, "#_promote_time", now);
    kv_int(&kv, "#_size", (long long)t->size);
    kv_int(&kv, "#_size_app_only", (long long)t->size);
    kv_int(&kv, "_install_sub_status", 1);
    kv_int(&kv, "_install_version", (long long)t->install_version);
    kv_str(&kv, "_local_concept_id", concept);
    kv_str(&kv, "_metadata_path", meta);
    snprintf(key, sizeof(key), "/user/app/%s", tid);
    kv_str(&kv, "_org_path", key);
    kv_int(&kv, "_path_icon0_info", (long long)0x9000000000000000ull);
    kv_int(&kv, "_path_icon0_info_time_stamp", t->icon_ts);
    kv_int(&kv, "_path_pic0_info", (long long)0xa000000000000000ull);
    kv_int(&kv, "_path_pic0_info_time_stamp", t->pic_ts);
    kv_int(&kv, "_primary_title_sort", 132101);
    kv_int(&kv, "_ps_platform", 1);
    kv_str(&kv, "_uninstall_recommend_atime", now);
    kv_int(&kv, "_uninstallable", 1);
    kv_int(&kv, "sync_index", app_sync);
    int internal_n = kv.n;
    kv_int(&kv, "#_access_index", access);
    kv_str(&kv, "#_last_access_time", now);
    kv_int(&kv, "_sort_priority", 100);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(idb, "INSERT OR REPLACE INTO tbl_appinfo (titleId,metaDataId,key,val) VALUES (?,?,?,?)", -1,
                           &st, NULL) != SQLITE_OK)
        goto out;
    for (int i = 0; i < kv.n; i++) {
        sqlite3_reset(st);
        sqlite3_bind_text(st, 1, tid, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, i < internal_n ? "prior:internal:0" : "prior", -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, kv.v[i].key, -1, SQLITE_STATIC);
        if (kv.v[i].is_int)
            sqlite3_bind_int64(st, 4, kv.v[i].i);
        else
            sqlite3_bind_text(st, 4, kv.v[i].s, -1, SQLITE_STATIC);
        if (sqlite3_step(st) != SQLITE_DONE) {
            printf("[db] appinfo %s: %s\n", kv.v[i].key, sqlite3_errmsg(idb));
            sqlite3_finalize(st);
            goto out;
        }
    }
    sqlite3_finalize(st);
    buf_t app_json = {0};
    kv_json_object(&app_json, &kv);

    /* concept info */
    static kvs_t ck;
    memset(&ck, 0, sizeof(ck));
    kv_str(&ck, "concept_name", t->title);
    for (int i = 0; i < t->sfo->count; i++)
        if (!strncmp(t->sfo->items[i].key, "TITLE_", 6) && strlen(t->sfo->items[i].key) == 8) {
            snprintf(key, sizeof(key), "concept_name_%s", t->sfo->items[i].key + 6);
            kv_str(&ck, key, t->sfo->items[i].sval);
        }
    kv_int(&ck, "content_type", 4);
    kv_int(&ck, "contents_location", 1);
    kv_int(&ck, "contents_status", 1);
    kv_str(&ck, "deeplink_uri", deeplink);
    kv_int(&ck, "display_location", 138);
    kv_int(&ck, "external_hdd_app_status", 1);
    kv_int(&ck, "hdd_location", 1);
    kv_str(&ck, "hubapp_uri", hub);
    kv_int(&ck, "install_status", 66049);
    kv_str(&ck, "install_time", now);
    kv_str(&ck, "last_access_date", now);
    kv_int(&ck, "last_access_index", access);
    kv_str(&ck, "metadata_path", meta);
    kv_int(&ck, "operation_capability", 2);
    kv_int(&ck, "path_icon0_info", (long long)0x9000000000000000ull);
    kv_int(&ck, "path_icon0_info_time_stamp", t->icon_ts);
    kv_int(&ck, "path_info", 0);
    kv_int(&ck, "path_info_2", 0);
    kv_int(&ck, "path_pic0_info", (long long)0xa000000000000000ull);
    kv_int(&ck, "path_pic0_info_time_stamp", t->pic_ts);
    kv_str(&ck, "primary_title_id", tid);
    kv_str(&ck, "promote_time", now);
    kv_int(&ck, "sort_priority", 100);
    kv_int(&ck, "sync_index", concept_sync);
    kv_int(&ck, "total_size", (long long)t->size);
    if (sqlite3_prepare_v2(idb, "INSERT OR REPLACE INTO tbl_conceptinfo (local_concept_id,metadata_id,key,val) VALUES (?,NULL,?,?)",
                           -1, &st, NULL) != SQLITE_OK)
        goto out_json;
    for (int i = 0; i < ck.n; i++) {
        sqlite3_reset(st);
        sqlite3_bind_text(st, 1, concept, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, ck.v[i].key, -1, SQLITE_STATIC);
        if (ck.v[i].is_int)
            sqlite3_bind_int64(st, 3, ck.v[i].i);
        else
            sqlite3_bind_text(st, 3, ck.v[i].s, -1, SQLITE_STATIC);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    buf_t concept_json = {0};
    kv_field_list(&concept_json, &ck);

    /* app.db */
    static row_t r;
    memset(&r, 0, sizeof(r));
    cs(&r, "titleId", tid); cs(&r, "contentId", cid); cs(&r, "titleName", t->title); cs(&r, "metaDataPath", meta);
    cs(&r, "lastAccessTime", now); ci(&r, "contentStatus", 0); ci(&r, "contentLocation", 0);
    ci(&r, "ps4parentalLevel", parental); ci(&r, "sortPriority", 100); ci(&r, "pathInfo", 0);
    ci(&r, "lastAccessIndex", access); ci(&r, "dispLocation", 138); ci(&r, "uninstallable", 1);
    ci(&r, "ps4contentType", 0); ci(&r, "pathInfo2", 0); ci(&r, "size", (long long)t->size);
    cs(&r, "promoteTime", now); cs(&r, "installTime", now); cs(&r, "viewCategory", "game");
    ci(&r, "ps4disableLiveDetail", 0); ci(&r, "hddLocation", 0); ci(&r, "externalHddAppStatus", 0);
    cs(&r, "mTime", now); ci(&r, "sizeOtherHdd", 0); ci(&r, "appFormatType", 0); ci(&r, "categoryType", 61440);
    ci(&r, "ps4Attribute", attribute); ci(&r, "attributeInternal", 0); ci(&r, "conceptId", 0);
    cs(&r, "localConceptId", concept); cs(&r, "pprDeeplinkUri", deeplink); cs(&r, "pprHubAppUri", hub);
    ci(&r, "platform", 1); ci(&r, "installStatus", 0); cs(&r, "icon0Info", icon); cs(&r, "pic0Info", pic);
    ci(&r, "discStatus", 0); ci(&r, "isTitleDiscInserted", 0); ci(&r, "appType", app_type);
    ci(&r, "primaryTitleSort", 132101); ci(&r, "appinfoContentType", 1); ci(&r, "contentBadgeType", 0);
    ci(&r, "is3rdPartyVideoApp", 0); ci(&r, "packageLocation", 1);
    if (insert_row(adb, "tbl_contentinfo", &r, "AppInfoJson", (const char *)app_json.data))
        goto out_all;

    memset(&r, 0, sizeof(r));
    cs(&r, "localConceptId", concept); ci(&r, "conceptId", 0); cs(&r, "conceptName", t->title);
    cs(&r, "metaDataPath", meta); cs(&r, "lastAccessTime", now); ci(&r, "lastAccessIndex", access);
    ci(&r, "sortPriority", 100); ci(&r, "pathInfo", 0); ci(&r, "pathInfo2", 0); ci(&r, "dispLocation", 138);
    ci(&r, "totalSize", (long long)t->size); ci(&r, "hddLocation", 1); ci(&r, "externalHddAppStatus", 1);
    ci(&r, "contentLocation", 1); ci(&r, "installStatus", 66049); cs(&r, "pprDeeplinkUri", deeplink);
    cs(&r, "pprHubAppUri", hub); cs(&r, "icon0Info", icon); cs(&r, "pic0Info", pic); ci(&r, "minimumAge", 0);
    ci(&r, "spaceType", 0); ci(&r, "classicGamePlatform", 0); ci(&r, "scpDataStored", 0);
    ci(&r, "mergedOperationCapability", 2); ci(&r, "mergedContentsStatus", 1); cs(&r, "mergedPromoteTime", now);
    cs(&r, "mergedInstallTime", now); ci(&r, "mergedAppInfoContentType", 4);
    if (insert_row(adb, "tbl_conceptmetadata", &r, "ConceptInfoJson", (const char *)concept_json.data))
        goto out_all;

    for (int u = 0; u < nusers; u++) {
        char table[64];
        memset(&r, 0, sizeof(r));
        cs(&r, "titleId", tid); cs(&r, "titleName", t->title); cs(&r, "localConceptId", concept);
        ci(&r, "conceptId", 0); cs(&r, "lastAccessTime", now); ci(&r, "lastAccessIndex", icon_access);
        cs(&r, "recentActivityDatePlayedOrInstalled", now); cs(&r, "installedDate", now); ci(&r, "installStatus", 2);
        ci(&r, "dispLocation", 138); ci(&r, "visible", 1); ci(&r, "externalLaunchFlag", 0); ci(&r, "psNowFlag", 0);
        cs(&r, "deeplinkUri", deeplink); cs(&r, "hubAppUri", hub); ci(&r, "platform", 1); ci(&r, "appDrmType", 5);
        ci(&r, "betaFlag", 0); ci(&r, "contentBadgeType", 0); ci(&r, "isVr", 0);
        ci(&r, "primaryTitleSort", 4295099397ll); ci(&r, "discStatus", 0); ci(&r, "lastLaunchMethod", 0);
        cs(&r, "contentId", cid); ci(&r, "entEntitlement", 0); ci(&r, "entFreePsPlusContent", 0);
        ci(&r, "entActiveFlag", 0); ci(&r, "entHidden", 0);
        snprintf(table, sizeof(table), "tbl_iconinfo_%010u", uids[u]);
        if (insert_row(adb, table, &r, NULL, NULL))
            goto out_all;

        memset(&r, 0, sizeof(r));
        cs(&r, "localConceptId", concept); ci(&r, "validFlag", 1); ci(&r, "conceptId", 0);
        cs(&r, "conceptName", t->title); ci(&r, "lastAccessIndex", icon_access); ci(&r, "totalPlayedTime", 0);
        cs(&r, "primaryTitleId", tid); cs(&r, "primaryTitleName", t->title); ci(&r, "primaryTitlePlatform", 1);
        cs(&r, "lastInteractedTime", now); ci(&r, "dispLocation", 1162); ci(&r, "filterTypeEntitlement", 0);
        ci(&r, "totalEstimatedSize", 0); cs(&r, "promoteTime", now); ci(&r, "psNowFlag", 0);
        cs(&r, "deeplinkUri", deeplink); cs(&r, "hubAppUri", hub); cs(&r, "metaDataPath", meta);
        cs(&r, "icon0Info", icon); cs(&r, "pic0Info", pic); ci(&r, "classicGamePlatform", 0); ci(&r, "minimumAge", 0);
        ci(&r, "genreFlags", 0); ci(&r, "spaceType", 0);
        snprintf(table, sizeof(table), "tbl_concepticoninfo_%010u", uids[u]);
        if (insert_row(adb, table, &r, NULL, NULL))
            goto out_all;
    }
    rc = 0;
out_all:
    buf_free(&concept_json);
out_json:
    buf_free(&app_json);
out:
    sqlite3_exec(idb, rc ? "ROLLBACK" : "COMMIT", NULL, NULL, NULL);
    sqlite3_exec(adb, rc ? "ROLLBACK" : "COMMIT", NULL, NULL, NULL);
    sqlite3_close(idb);
    sqlite3_close(adb);
    return rc;
}

int db_unregister(const char *title_id) {
    sqlite3 *adb = NULL, *idb = NULL;
    if (open_db(APP_DB, &adb) || open_db(APPINFO_DB, &idb)) {
        sqlite3_close(adb);
        return -1;
    }
    char concept[64], sql[256];
    snprintf(concept, sizeof(concept), "cid:local:%s", title_id);
    sqlite3_exec(adb, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    unsigned uids[MAX_USERS];
    int nusers = user_tables(adb, uids);
    snprintf(sql, sizeof(sql), "DELETE FROM tbl_contentinfo WHERE titleId='%s'", title_id);
    sqlite3_exec(adb, sql, NULL, NULL, NULL);
    snprintf(sql, sizeof(sql), "DELETE FROM tbl_conceptmetadata WHERE localConceptId='%s'", concept);
    sqlite3_exec(adb, sql, NULL, NULL, NULL);
    for (int u = 0; u < nusers; u++) {
        snprintf(sql, sizeof(sql), "DELETE FROM tbl_iconinfo_%010u WHERE titleId='%s'", uids[u], title_id);
        sqlite3_exec(adb, sql, NULL, NULL, NULL);
        snprintf(sql, sizeof(sql), "DELETE FROM tbl_concepticoninfo_%010u WHERE primaryTitleId='%s'", uids[u], title_id);
        sqlite3_exec(adb, sql, NULL, NULL, NULL);
    }
    sqlite3_exec(adb, "COMMIT", NULL, NULL, NULL);
    sqlite3_exec(idb, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    snprintf(sql, sizeof(sql), "DELETE FROM tbl_conceptinfo WHERE local_concept_id='%s'", concept);
    sqlite3_exec(idb, sql, NULL, NULL, NULL);
    snprintf(sql, sizeof(sql), "DELETE FROM tbl_appinfo WHERE titleId='%s'", title_id);
    sqlite3_exec(idb, sql, NULL, NULL, NULL);
    sqlite3_exec(idb, "COMMIT", NULL, NULL, NULL);
    sqlite3_close(idb);
    sqlite3_close(adb);
    return 0;
}
