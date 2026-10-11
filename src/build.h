/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Builders for install files. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pkg.h"

typedef struct {
    uint8_t *data;
    size_t len, cap;
} buf_t;

void buf_put(buf_t *b, const void *data, size_t len);
void buf_printf(buf_t *b, const char *fmt, ...);
void buf_free(buf_t *b);

/* /user/app/<TID>/app.json; url is the pkg path as the console sees it. */
void build_app_json(buf_t *out, const pkg_t *pkg, const char *url);
/* /user/app/<TID>/app.pbm or /user/patch/<TID>/patch.pbm (+ .backup). */
void build_app_pbm(buf_t *out, const pkg_t *pkg, const char *app_ver, int is_patch);
/* /user/app/<TID>/app.xml from playgo-chunk.dat. */
int build_app_xml(buf_t *out, const uint8_t *playgo, size_t len);
/* Update side of a title, all NULL/0 when there is no patch. */
typedef struct {
    const pkg_t *pkg;
    const uint8_t *playgo;
    size_t playgo_len;
    const char *ver, *url;
    const buf_t *json;
} patch_ref_t;

/* /system_data/playgo/<TID>/pfs.pbm */
int build_pfs_pbm(buf_t *out, const pkg_t *pkg, const uint8_t *playgo, size_t len, const patch_ref_t *patch);
/* /user/playgo/app/<CID>/00/d0.pdb (and d1.pdb); ver is the effective (patch) version. */
int build_pdb(buf_t *out, const pkg_t *pkg, const char *ver, const char *url, const buf_t *app_json,
              const uint8_t *playgo, size_t len, const patch_ref_t *patch);
/* /user/playgo/patch/<CID>/00/00.00/d0.pdb (and d1.pdb). */
int build_patch_pdb(buf_t *out, const patch_ref_t *patch);
/* /user/license/fake<CID[:19]>.rif and .idx */
void build_license(buf_t *rif, buf_t *idx, const pkg_t *pkg, const uint8_t *license_dat);

uint16_t playgo_chunk_count(const uint8_t *playgo, size_t len);
