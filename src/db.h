/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * app.db / appinfo.db access. */
#pragma once

#include <stdint.h>

#include "pkg.h"

typedef struct {
    const pkg_t *pkg;
    const sfo_t *sfo;       /* patch param.sfo when an update is present */
    const char *title;      /* TITLE from param.sfo */
    uint64_t size;          /* app + patch pkg sizes */
    int64_t icon_ts, pic_ts; /* appmeta file mtimes, 0 when missing */
    uint64_t install_version;
} db_title_t;

int db_is_registered(const char *title_id);
/* Inserts rows into app.db and appinfo.db. Returns 0 on success. */
int db_register(const db_title_t *t);
/* Removes every row of title_id from app.db and appinfo.db. */
int db_unregister(const char *title_id);
