/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * PS4Mount daemon: scans USB drives, registers PS4 fpkgs without installing them, follows klog and serves the web panel. */
/* ps4mount: run PS4 fpkgs from external storage without installing them.
 *
 * Every scan:
 *   1. A PS4 app pkg found at the root of /mnt/usbN or /mnt/extN is moved to
 *      <root>/ps4/<TITLE_ID>/app.pkg (same disk, rename only).
 *   2. For each <root>/ps4/<TITLE_ID>/app.pkg the files a real install leaves
 *      behind are generated from the pkg into an internal folder next to a symlink
 *      to the pkg, that folder is mounted over /user/app/<TITLE_ID> (nullfs) and
 *      the title is added to app.db/appinfo.db.
 *   3. Mounts whose storage disappeared are released.
 * Titles installed the normal way are never touched. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "build.h"
#include "db.h"
#include "pkg.h"
#include "web.h"
#include "icon_data.h"

#define VERSION "PS4Mount 0.3.4"
#define LOCK_PORT 9032
#define SCAN_SECONDS 10
#define STATE_DIR "/data/ps4mount"
/* Internal folders mounted over /user/app/<TID> and /user/patch/<TID>: the files an
 * install leaves there plus app.pkg/patch.pkg as symlinks to the pkgs on the drive.
 * /user is bfs, which has no symlinks; /system_data is ufs. A delete from the
 * console only ever removes these links, never the pkgs. */
#define HOLD_DIR "/system_data/ps4mount"
#define MAX_TITLES 256

typedef struct { char useless[45]; char message[3075]; } notify_request_t;
int sceUserServiceInitialize(void *);
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallAll(void *);
int sceAppInstUtilAppUnInstall(const char *);
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static void notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void notify(const char *fmt, ...) {
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    va_end(ap);
    printf("[notify] %s\n", req.message);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* --- small fs helpers --- */

static int exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static void mkdirs(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, mode);
            *p = '/';
        }
    mkdir(tmp, mode);
}

static int write_file(const char *path, const void *data, size_t len, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0)
        return -1;
    const uint8_t *p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    close(fd);
    return 0;
}

static int mounted_from(const char *mountpoint, char *from, size_t n) {
    struct statfs sf;
    if (statfs(mountpoint, &sf) || strcmp(sf.f_mntonname, mountpoint) || strcmp(sf.f_fstypename, "nullfs"))
        return 0;
    if (from)
        snprintf(from, n, "%s", sf.f_mntfromname);
    return 1;
}

static int is_mountpoint(const char *path) {
    struct statfs sf;
    return statfs(path, &sf) == 0 && !strcmp(sf.f_mntonname, path);
}

/* --- appmeta entries copied verbatim from the pkg --- */

typedef struct {
    uint32_t id;
    const char *name;
} meta_entry_t;

static const meta_entry_t k_user_meta[] = {
    {0x1200, "icon0.png"}, {0x1220, "pic0.png"},  {0x1006, "pic1.png"},          {0x1240, "snd0.at9"},
    {0x1280, "icon0.dds"}, {0x12a0, "pic0.dds"},  {0x1004, "pronunciation.xml"}, {0x100b, "shareparam.json"},
    {0, NULL},
};

static int write_entry(pkg_t *pkg, uint32_t id, const char *path) {
    size_t len;
    uint8_t *data = pkg_entry(pkg, id, &len);
    if (!data)
        return 1; /* absent entry is fine */
    int rc = write_file(path, data, len, 0777);
    free(data);
    return rc;
}

static int write_buf(const char *path, const buf_t *b) { return write_file(path, b->data, b->len, 0777); }

/* --- registration --- */

/* One library folder: <root>/ps4/<TID>/app.pkg plus an optional patch.pkg. */
typedef struct {
    char dir[PATH_MAX], app_path[PATH_MAX], patch_path[PATH_MAX];
    pkg_t app, patch;
    int has_patch;
    sfo_t app_sfo, patch_sfo;
    char name[128], ver[8], marker[160];
    char update_issue[256]; /* why patch.pkg was ignored, empty when used or absent */
} title_src_t;

static int read_sfo(pkg_t *pkg, sfo_t *sfo) {
    size_t len;
    uint8_t *d = pkg_entry(pkg, 0x1000, &len);
    int rc = d ? sfo_parse(d, len, sfo) : -1;
    free(d);
    return rc;
}

static void src_close(title_src_t *t) {
    pkg_close(&t->app);
    if (t->has_patch)
        pkg_close(&t->patch);
}

static int first_time(const char *path);

static int src_open(title_src_t *t, const char *root, const char *tid) {
    memset(t, 0, sizeof(*t));
    snprintf(t->dir, sizeof(t->dir), "%s/ps4/%s", root, tid);
    snprintf(t->app_path, sizeof(t->app_path), "%s/app.pkg", t->dir);
    snprintf(t->patch_path, sizeof(t->patch_path), "%s/patch.pkg", t->dir);
    if (pkg_open(&t->app, t->app_path) || read_sfo(&t->app, &t->app_sfo)) {
        printf("[scan] %s: unreadable app pkg\n", t->app_path);
        return -1;
    }
    if (exists(t->patch_path)) {
        const sfo_item_t *gv = sfo_get(&t->app_sfo, "VERSION");
        if (pkg_open(&t->patch, t->patch_path) || read_sfo(&t->patch, &t->patch_sfo)) {
            snprintf(t->update_issue, sizeof(t->update_issue), "Update ignored: patch.pkg could not be read");
            if (t->patch.fd > 0)
                pkg_close(&t->patch);
        } else {
            /* An update only fits the title it was made for. (Master VERSION may differ:
             * installed games run with base 01.02 + update 01.00.) */
            const sfo_item_t *uv = sfo_get(&t->patch_sfo, "VERSION");
            const char *gver = gv ? gv->sval : "?", *uver = uv ? uv->sval : "?";
            if (strcmp(t->patch.title_id, tid)) {
                snprintf(t->update_issue, sizeof(t->update_issue),
                         "Update ignored: it is for %s version %s, the game is %s version %s", t->patch.title_id, uver,
                         tid, gver);
                pkg_close(&t->patch);
            } else {
                t->has_patch = 1;
            }
        }
        if (t->update_issue[0] && first_time(t->patch_path)) {
            const sfo_item_t *title = sfo_get(&t->app_sfo, "TITLE");
            notify("%s: %s", title ? title->sval : tid, t->update_issue);
        }
    }
    const sfo_t *eff = t->has_patch ? &t->patch_sfo : &t->app_sfo;
    const sfo_item_t *it = sfo_get(eff, "TITLE");
    snprintf(t->name, sizeof(t->name), "%s", it ? it->sval : tid);
    it = sfo_get(eff, "APP_VER");
    snprintf(t->ver, sizeof(t->ver), "%s", it && !it->is_int ? it->sval : "01.00");
    char digest[65] = "";
    if (t->has_patch)
        for (int i = 0; i < 32; i++)
            snprintf(digest + i * 2, 3, "%02x", t->patch.digest[i]);
    snprintf(t->marker, sizeof(t->marker), "%s\n%s\n", t->dir, t->has_patch ? digest : "nopatch");
    return 0;
}

static void write_meta(pkg_t *pkg, const char *meta_dir, int *rc) {
    char q[PATH_MAX];
    for (const meta_entry_t *m = k_user_meta; m->name; m++) {
        snprintf(q, sizeof(q), "%s/%s", meta_dir, m->name);
        *rc |= write_entry(pkg, m->id, q) < 0;
    }
    for (uint32_t id = 0x1260; id <= 0x127f; id++) {
        size_t len;
        uint8_t *d = pkg_entry(pkg, id, &len);
        if (!d)
            continue;
        snprintf(q, sizeof(q), "%s/changeinfo", meta_dir);
        mkdirs(q, 0777);
        if (id == 0x1260)
            snprintf(q, sizeof(q), "%s/changeinfo/changeinfo.xml", meta_dir);
        else
            snprintf(q, sizeof(q), "%s/changeinfo/changeinfo_%02u.xml", meta_dir, id - 0x1261);
        *rc |= write_file(q, d, len, 0777);
        free(d);
    }
}

static void rm_tree(const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        unlink(path);
        return;
    }
    struct dirent *e;
    char p[PATH_MAX];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
        struct stat st;
        if (!lstat(p, &st) && S_ISDIR(st.st_mode))
            rm_tree(p);
        else
            unlink(p);
    }
    closedir(d);
    rmdir(path);
}

static void hold_path(char *out, size_t n, const char *kind, const char *tid) {
    snprintf(out, n, HOLD_DIR "/%s/%s", kind, tid);
}

/* Fresh internal folder for kind ("app"/"patch") with name -> pkg_path. */
static int hold_prepare(const char *kind, const char *tid, const char *name, const char *pkg_path) {
    char dir[PATH_MAX], link[PATH_MAX];
    hold_path(dir, sizeof(dir), kind, tid);
    mkdirs(dir, 0777);
    snprintf(link, sizeof(link), "%s/%s", dir, name);
    unlink(link);
    if (symlink(pkg_path, link)) {
        printf("[reg] symlink %s: %s\n", link, strerror(errno));
        return -1;
    }
    return 0;
}

/* The link of kind exists and reaches its pkg. */
static int hold_ready(const char *kind, const char *tid, const char *name) {
    char p[PATH_MAX];
    hold_path(p, sizeof(p), kind, tid);
    snprintf(p + strlen(p), sizeof(p) - strlen(p), "/%s", name);
    return exists(p);
}

static int place_files(title_src_t *t) {
    pkg_t *app = &t->app, *pat = t->has_patch ? &t->patch : NULL;
    const char *tid = app->title_id, *cid = app->content_id;
    const sfo_item_t *av = sfo_get(&t->app_sfo, "APP_VER");
    const char *app_ver = av && !av->is_int ? av->sval : "01.00";
    char p[PATH_MAX], q[PATH_MAX], hold[PATH_MAX];
    int rc = 0;

    snprintf(p, sizeof(p), "/user/appmeta/%s", tid);
    mkdirs(p, 0777);
    write_meta(app, p, &rc);
    if (pat)
        write_meta(pat, p, &rc); /* the update's artwork wins, as with a real install */

    snprintf(p, sizeof(p), "/system_data/priv/appmeta/%s", tid);
    mkdirs(p, 0777);
    snprintf(q, sizeof(q), "%s/param.sfo", p);
    rc |= write_entry(pat ? pat : app, 0x1000, q) != 0;
    snprintf(q, sizeof(q), "%s/nptitle.dat", p);
    rc |= write_entry(app, 0x402, q) < 0;
    snprintf(q, sizeof(q), "%s/npbind.dat", p);
    rc |= write_entry(app, 0x403, q) < 0;

    size_t lic_len;
    uint8_t *lic = pkg_entry(app, 0x400, &lic_len);
    if (!lic || lic_len != 0x400) {
        free(lic);
        printf("[reg] %s: no license.dat\n", tid);
        return -1;
    }
    buf_t rif = {0}, idx = {0};
    build_license(&rif, &idx, app, lic);
    free(lic);
    snprintf(q, sizeof(q), "/user/license/fake%.19s.rif", cid);
    rc |= write_buf(q, &rif);
    snprintf(q, sizeof(q), "/user/license/fake%.19s.idx", cid);
    rc |= write_buf(q, &idx);
    buf_free(&rif);
    buf_free(&idx);

    size_t pl = 0, ppl = 0;
    uint8_t *pg = pkg_entry(app, 0x1001, &pl), *ppg = pat ? pkg_entry(pat, 0x1001, &ppl) : NULL;
    buf_t json = {0}, pbm = {0}, xml = {0}, pjson = {0}, ppbm_patch = {0}, pxml = {0}, ppbm = {0}, pdb = {0}, patch_pdb = {0};
    build_app_json(&json, app, t->app_path);
    build_app_pbm(&pbm, app, app_ver, 0);
    patch_ref_t pr = {0}, *prp = NULL;
    if (pat) {
        build_app_json(&pjson, pat, t->patch_path);
        build_app_pbm(&ppbm_patch, pat, t->ver, 1);
        pr = (patch_ref_t){pat, ppg, ppl, t->ver, t->patch_path, &pjson};
        prp = &pr;
    }
    if (build_app_xml(&xml, pg, pl) || build_pfs_pbm(&ppbm, app, pg, pl, prp) ||
        build_pdb(&pdb, app, t->ver, t->app_path, &json, pg, pl, prp) ||
        (pat && (build_app_xml(&pxml, ppg, ppl) || build_patch_pdb(&patch_pdb, &pr)))) {
        printf("[reg] %s: playgo-chunk.dat unusable\n", tid);
        rc = -1;
        goto out;
    }
    rc |= hold_prepare("app", tid, "app.pkg", t->app_path);
    hold_path(hold, sizeof(hold), "app", tid);
    snprintf(q, sizeof(q), "%s/app.json", hold);
    rc |= write_buf(q, &json);
    snprintf(q, sizeof(q), "%s/app.pbm", hold);
    rc |= write_buf(q, &pbm);
    snprintf(q, sizeof(q), "%s/app.pbm.backup", hold);
    rc |= write_buf(q, &pbm);
    snprintf(q, sizeof(q), "%s/app.xml", hold);
    rc |= write_buf(q, &xml);
    snprintf(p, sizeof(p), "/system_data/playgo/%s", tid);
    mkdirs(p, 0777);
    snprintf(q, sizeof(q), "%s/pfs.pbm", p);
    rc |= write_buf(q, &ppbm);
    snprintf(q, sizeof(q), "%s/pfs.pbm.backup", p);
    rc |= write_buf(q, &ppbm);
    snprintf(p, sizeof(p), "/user/playgo/app/%s/00", cid);
    mkdirs(p, 0777);
    snprintf(q, sizeof(q), "%s/d0.pdb", p);
    rc |= write_buf(q, &pdb);
    snprintf(q, sizeof(q), "%s/d1.pdb", p);
    rc |= write_buf(q, &pdb);
    snprintf(p, sizeof(p), "/user/playgo/patch/%s", cid);
    if (pat) {
        rc |= hold_prepare("patch", tid, "patch.pkg", t->patch_path);
        hold_path(hold, sizeof(hold), "patch", tid);
        snprintf(q, sizeof(q), "%s/patch.json", hold);
        rc |= write_buf(q, &pjson);
        snprintf(q, sizeof(q), "%s/patch.pbm", hold);
        rc |= write_buf(q, &ppbm_patch);
        snprintf(q, sizeof(q), "%s/patch.pbm.backup", hold);
        rc |= write_buf(q, &ppbm_patch);
        snprintf(q, sizeof(q), "%s/patch.xml", hold);
        rc |= write_buf(q, &pxml);
        snprintf(p, sizeof(p), "/user/playgo/patch/%s/00/00.00", cid);
        mkdirs(p, 0777);
        snprintf(q, sizeof(q), "%s/d0.pdb", p);
        rc |= write_buf(q, &patch_pdb);
        snprintf(q, sizeof(q), "%s/d1.pdb", p);
        rc |= write_buf(q, &patch_pdb);
    } else {
        rm_tree(p);
        hold_path(hold, sizeof(hold), "patch", tid);
        rm_tree(hold);
    }
out:
    free(pg);
    free(ppg);
    buf_free(&json);
    buf_free(&pbm);
    buf_free(&xml);
    buf_free(&pjson);
    buf_free(&ppbm_patch);
    buf_free(&pxml);
    buf_free(&ppbm);
    buf_free(&pdb);
    buf_free(&patch_pdb);
    return rc ? -1 : 0;
}

#define IOV(s) {(void *)(s), strlen(s) + 1}

/* Titles this daemon serves. */
typedef struct {
    int used, has_patch, running;
    time_t running_since; /* launch seen in klog; the sandbox mount appears shortly after */
    char tid[10], cid[37], name[128], dir[PATH_MAX];
} managed_t;

static managed_t g_managed[MAX_TITLES];
static int g_manual_scan; /* "Scan for games" in the panel */
static int g_deleting; /* a delete screen is open: game folders are unmounted */
static time_t g_deleting_since;
static time_t g_deleting_close_at; /* set when the uninstall finished */

static managed_t *managed_by(const char *tid, const char *cid) {
    for (int i = 0; i < MAX_TITLES; i++)
        if (g_managed[i].used && ((tid && !strcmp(g_managed[i].tid, tid)) || (cid && !strcmp(g_managed[i].cid, cid))))
            return &g_managed[i];
    return NULL;
}

/* Top mount on target: 0 none, 1 our folder, -1 something else (like a USB
 * folder mounted by an older PS4Mount). */
static int top_layer(const char *target, const char *dir) {
    struct statfs sf;
    if (statfs(target, &sf) || strcmp(sf.f_mntonname, target))
        return 0;
    return strcmp(sf.f_fstypename, "nullfs") || strcmp(sf.f_mntfromname, dir) ? -1 : 1;
}

static int nullfs_mount(const char *dir, const char *target) {
    struct iovec iov[] = {IOV("fstype"), IOV("nullfs"), IOV("from"), IOV(dir), IOV("fspath"), IOV(target)};
    if (nmount(iov, 6, 0)) {
        printf("[mount] %s -> %s: %s\n", dir, target, strerror(errno));
        return -1;
    }
    return 0;
}

/* A running PS4 title keeps /mnt/sandbox/pfsmnt/<TID>-app0 mounted. Unlike the klog
 * events this survives a restart of the daemon, so it is the reference for "running". */
static int title_running(const char *tid) {
    char want[64];
    snprintf(want, sizeof(want), "/mnt/sandbox/pfsmnt/%s-app0", tid);
    int n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n <= 0)
        return 0;
    struct statfs *list = malloc(sizeof(*list) * (size_t)(n + 16));
    if (!list)
        return 0;
    n = getfsstat(list, (long)(sizeof(*list) * (size_t)(n + 16)), MNT_NOWAIT);
    int found = 0;
    for (int i = 0; i < n && !found; i++)
        found = !strcmp(list[i].f_mntonname, want);
    free(list);
    return found;
}

static int mount_dir(const char *dir, const char *target) {
    mkdir(target, 0777);
    int top = top_layer(target, dir);
    if (top == 1)
        return 0;
    while (top != 0) {
        if (unmount(target, 0)) {
            printf("[mount] unmount %s: %s\n", target, strerror(errno));
            return -1;
        }
        top = top_layer(target, dir);
    }
    return nullfs_mount(dir, target);
}

static void unmount_all_layers(const char *target, int force) {
    for (int i = 0; i < 8 && mounted_from(target, NULL, 0); i++)
        if (unmount(target, force ? MNT_FORCE : 0)) {
            printf("[mount] unmount %s: %s\n", target, strerror(errno));
            if (!force)
                return;
        }
}

/* Removes every layer of the title (forced: a system uninstall is about to run). */
static void unmount_title(const char *tid) {
    char target[64];
    snprintf(target, sizeof(target), "/user/app/%s", tid);
    unmount_all_layers(target, 1);
    snprintf(target, sizeof(target), "/user/patch/%s", tid);
    unmount_all_layers(target, 1);
}

/* /user/app/<TID> always, /user/patch/<TID> only with an update. */
static int mount_paths(const char *tid, int has_patch) {
    char target[64], hold[PATH_MAX];
    snprintf(target, sizeof(target), "/user/app/%s", tid);
    hold_path(hold, sizeof(hold), "app", tid);
    if (mount_dir(hold, target))
        return -1;
    snprintf(target, sizeof(target), "/user/patch/%s", tid);
    hold_path(hold, sizeof(hold), "patch", tid);
    if (has_patch)
        return mount_dir(hold, target);
    unmount_all_layers(target, 1);
    return 0;
}

static int mount_title(const title_src_t *t) { return mount_paths(t->app.title_id, t->has_patch); }

static void manage(const title_src_t *t) {
    managed_t *m = managed_by(t->app.title_id, NULL);
    for (int i = 0; !m && i < MAX_TITLES; i++)
        if (!g_managed[i].used)
            m = &g_managed[i];
    if (!m)
        return;
    /* keep the runtime state across rescans */
    managed_t keep = *m;
    memset(m, 0, sizeof(*m));
    m->used = 1;
    if (keep.used) {
        m->running = keep.running;
        m->running_since = keep.running_since;
    }
    m->has_patch = t->has_patch;
    snprintf(m->tid, sizeof(m->tid), "%s", t->app.title_id);
    snprintf(m->cid, sizeof(m->cid), "%s", t->app.content_id);
    snprintf(m->name, sizeof(m->name), "%s", t->name);
    snprintf(m->dir, sizeof(m->dir), "%s", t->dir);
}


static int removed(const char *tid) {
    char p[64];
    snprintf(p, sizeof(p), STATE_DIR "/%s.removed", tid);
    return exists(p);
}

/* Real installs keep app.pkg on the internal drive; never touch those. */
static int installed_internally(const char *tid) {
    char p[64];
    snprintf(p, sizeof(p), "/user/app/%s", tid);
    if (mounted_from(p, NULL, 0))
        return 0;
    snprintf(p, sizeof(p), "/user/app/%s/app.pkg", tid);
    return exists(p);
}

static int marker_matches(const char *tid, const char *expected) {
    char path[64], buf[256] = {0};
    snprintf(path, sizeof(path), STATE_DIR "/%s.src", tid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n > 0 && !strcmp(buf, expected);
}

/* 1 = needs (re)registration, 0 = registered here, -1 = skip. */
/* The title is served from another connected drive: the marker names another
 * folder whose game pkg is still there. The first drive keeps it, so the same game
 * on two drives is not switched (and registered again) on every scan. */
static int served_elsewhere(const title_src_t *t) {
    char path[64], buf[PATH_MAX + 256] = {0}, pkg[PATH_MAX + 16];
    snprintf(path, sizeof(path), STATE_DIR "/%s.src", t->app.title_id);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[strcspn(buf, "\n")] = 0;
    if (!strcmp(buf, t->dir))
        return 0;
    snprintf(pkg, sizeof(pkg), "%s/app.pkg", buf);
    return exists(pkg);
}

static int title_state(title_src_t *t) {
    const char *tid = t->app.title_id;
    if (installed_internally(tid) || removed(tid))
        return -1;
    if (served_elsewhere(t)) {
        if (first_time(t->app_path))
            printf("[scan] %s: %s is already served from another drive, this copy is not used\n", t->dir, tid);
        return -1;
    }
    char pdb[PATH_MAX];
    snprintf(pdb, sizeof(pdb), "/user/playgo/app/%s/00/d0.pdb", t->app.content_id);
    /* folders set up by 0.3.1 and older lack the internal folder: registered again */
    return !db_is_registered(tid) || !exists(pdb) || !marker_matches(tid, t->marker) ||
           !hold_ready("app", tid, "app.pkg") || (t->has_patch && !hold_ready("patch", tid, "patch.pkg"));
}

static void pump_events(int ms);

/* The launch check compares /system_data/priv/appmeta/<TID>/param.sfo with the
 * update's param.sfo byte for byte; AppInstallAll writes the game's one there. */
static void enforce_patch_sfo(title_src_t *t) {
    if (!t->has_patch)
        return;
    size_t len;
    uint8_t *sfo = pkg_entry(&t->patch, 0x1000, &len);
    if (!sfo)
        return;
    char path[96];
    snprintf(path, sizeof(path), "/system_data/priv/appmeta/%s/param.sfo", t->app.title_id);
    int fd = open(path, O_RDONLY), same = 0;
    if (fd >= 0) {
        uint8_t *cur = malloc(len + 1);
        same = cur && read(fd, cur, len + 1) == (ssize_t)len && !memcmp(cur, sfo, len);
        free(cur);
        close(fd);
    }
    if (!same) {
        printf("[reg] %s: priv param.sfo was not the update's, rewritten\n", t->app.title_id);
        write_file(path, sfo, len, 0777);
    }
    free(sfo);
}

/* sceAppInstUtilAppInstallAll runs in its own thread: after rest mode it was seen to
 * block for good, which froze the whole daemon (panel, klog, stop request). A call
 * still running is never doubled; titles keep being checked in the database. */
static volatile int g_install_busy, g_install_rc;

static void *install_thread(void *arg) {
    (void)arg;
    g_install_rc = sceAppInstUtilAppInstallAll(NULL);
    g_install_busy = 0;
    return NULL;
}

static int install_all_start(void) {
    if (g_install_busy) {
        printf("[reg] AppInstallAll still running from an earlier call\n");
        return -1;
    }
    g_install_busy = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, install_thread, NULL)) {
        g_install_busy = 0;
        g_install_rc = sceAppInstUtilAppInstallAll(NULL); /* no thread: call it here */
        return 0;
    }
    pthread_detach(th);
    return 0;
}

static int register_title(title_src_t *t) {
    const char *tid = t->app.title_id;
    if (place_files(t) || mount_title(t))
        return -1;
    manage(t);
    /* New title: let ShellCore register everything under /user/app itself.
     * That path also refreshes the home screen and fetches the PSN concept. */
    if (!db_is_registered(tid)) {
        install_all_start();
        /* the icon shows up before the rows land: keep serving klog/panel meanwhile */
        for (int i = 0; i < 30 && !db_is_registered(tid); i++)
            pump_events(1000);
        printf("[reg] %s: AppInstallAll=%s0x%08x registered=%d\n", tid, g_install_busy ? "(still running) " : "",
               g_install_rc, db_is_registered(tid));
    }
    /* The system databases are only ever written by the system: ShellCore keeps them
     * open with its own cache, and rows written from outside make it flag app.db as
     * corrupted (and rebuild it at the next boot). A title the system did not
     * register is tried again on the next scan. */
    if (!db_is_registered(tid)) {
        printf("[reg] %s: not registered by the system yet, retried on the next scan\n", tid);
        return -1;
    }
    enforce_patch_sfo(t);
    char marker[64];
    snprintf(marker, sizeof(marker), STATE_DIR "/%s.src", tid);
    if (write_file(marker, t->marker, strlen(t->marker), 0666))
        return -1;
    manage(t);
    return 0;
}

/* Logs a skipped root file once per daemon run. */
static int first_time(const char *path) {
    static char seen[64][PATH_MAX];
    static int n;
    for (int i = 0; i < n; i++)
        if (!strcmp(seen[i], path))
            return 0;
    if (n < 64)
        snprintf(seen[n++], PATH_MAX, "%s", path);
    return 1;
}

/* Moves PS4 game (gd) and update (gp) pkgs from the storage root into
 * <root>/ps4/<TID>/app.pkg and patch.pkg. */
static int adopt_root_pkgs(const char *root) {
    int moved = 0;
    DIR *d = opendir(root);
    if (!d)
        return 0;
    struct dirent *e;
    char path[PATH_MAX], dst[PATH_MAX];
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcasecmp(e->d_name + n - 4, ".pkg"))
            continue;
        snprintf(path, sizeof(path), "%s/%s", root, e->d_name);
        pkg_t pkg;
        if (pkg_open(&pkg, path))
            continue;
        sfo_t sfo;
        const sfo_item_t *cat = NULL;
        if (!read_sfo(&pkg, &sfo))
            cat = sfo_get(&sfo, "CATEGORY");
        int is_app = cat && !strcmp(cat->sval, "gd"), is_patch = cat && !strcmp(cat->sval, "gp");
        int ok = (is_app || is_patch) && !strncmp(pkg.title_id, "CUSA", 4) && pkg_fake_key(&pkg);
        char tid[10], ver[16] = "";
        uint8_t digest[32];
        snprintf(tid, sizeof(tid), "%s", pkg.title_id);
        memcpy(digest, pkg.digest, sizeof(digest));
        const sfo_item_t *av = ok ? sfo_get(&sfo, "APP_VER") : NULL;
        if (av && !av->is_int)
            snprintf(ver, sizeof(ver), "%s", av->sval);
        pkg_close(&pkg);
        if (!ok) {
            if (first_time(path))
                printf("[scan] skip %s (not a fake PS4 game or update pkg)\n", path);
            continue;
        }
        if (installed_internally(tid)) {
            if (first_time(path))
                printf("[scan] skip %s (%s is installed internally)\n", path, tid);
            continue;
        }
        snprintf(dst, sizeof(dst), "%s/ps4/%s", root, tid);
        mkdirs(dst, 0777);
        snprintf(dst, sizeof(dst), "%s/ps4/%s/%s", root, tid, is_app ? "app.pkg" : "patch.pkg");
        if (exists(dst)) {
            pkg_t cur;
            int same = !pkg_open(&cur, dst) && !memcmp(cur.digest, digest, sizeof(digest));
            if (cur.fd > 0)
                pkg_close(&cur);
            if (same || is_app) {
                /* duplicates, and a second copy of a game, stay where they are */
                if (first_time(path))
                    notify(same ? "%s is already in use for %s; the copy was left in the USB root"
                                : "%s: %s already has a game package; the new one was left in the USB root",
                           e->d_name, tid);
                continue;
            }
            /* a different update: it replaces the current one, which is kept aside */
            char old[PATH_MAX];
            snprintf(old, sizeof(old), "%s/ps4/%s/replaced", root, tid);
            mkdirs(old, 0777);
            snprintf(old, sizeof(old), "%s/ps4/%s/replaced/patch-%ld.pkg", root, tid, (long)time(NULL));
            if (rename(dst, old)) {
                printf("[scan] keep %s aside: %s\n", dst, strerror(errno));
                continue;
            }
            notify("New update for %s (v%s): the previous one was moved to ps4/%s/replaced", tid, ver, tid);
        }
        if (rename(path, dst)) {
            printf("[scan] move %s: %s\n", path, strerror(errno));
        } else {
            printf("[scan] moved %s -> %s\n", path, dst);
            moved++;
        }
    }
    closedir(d);
    return moved;
}

typedef struct {
    char tid[10];
    int state;
} title_t;

/* A ps4/ folder or a .pkg in the root. Other partitions (like the boot partition
 * Ventoy adds to a drive) are skipped without a notification. */
static int has_candidates(const char *root) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/ps4", root);
    if (exists(p))
        return 1;
    DIR *d = opendir(root);
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        found = n > 4 && !strcasecmp(e->d_name + n - 4, ".pkg");
    }
    closedir(d);
    return found;
}

/* Titles that failed to be added wait before the next try, and the failure is
 * notified once: when the system refuses registrations, every scan would retry. */
#define RETRY_SECONDS 300

typedef struct {
    char tid[10];
    time_t until;
} retry_t;

static retry_t g_retry[MAX_TITLES];

static retry_t *retry_entry(const char *tid, int create) {
    retry_t *free_slot = NULL;
    for (int i = 0; i < MAX_TITLES; i++) {
        if (!strcmp(g_retry[i].tid, tid))
            return &g_retry[i];
        if (!free_slot && !g_retry[i].tid[0])
            free_slot = &g_retry[i];
    }
    if (create && free_slot)
        snprintf(free_slot->tid, sizeof(free_slot->tid), "%s", tid);
    return create ? free_slot : NULL;
}

static int retry_waiting(const char *tid) {
    retry_t *r = retry_entry(tid, 0);
    return r && time(NULL) < r->until;
}

/* Returns 1 the first time a title fails (to notify it), 0 for repeated failures. */
static int retry_failed(const char *tid) {
    retry_t *r = retry_entry(tid, 1);
    if (!r)
        return 1;
    int first = !r->until;
    r->until = time(NULL) + RETRY_SECONDS;
    printf("[reg] %s: next try in %d seconds\n", tid, RETRY_SECONDS);
    return first;
}

static void retry_clear(const char *tid) {
    retry_t *r = retry_entry(tid, 0);
    if (r)
        memset(r, 0, sizeof(*r));
}

static void scan_root(const char *root, int fresh, int reconnected) {
    const char *label = root + 5; /* "usb0" / "ext0" */
    if (!has_candidates(root)) {
        if (fresh)
            printf("[scan] %s: no ps4 folder or pkg, skipped\n", root);
        return;
    }
    if (fresh)
        notify("Scanning %s for PS4 games...", label);
    int moved = adopt_root_pkgs(root);
    if (moved && !fresh)
        notify("%s: %d new package(s) found", label, moved);
    char lib[PATH_MAX];
    snprintf(lib, sizeof(lib), "%s/ps4", root);
    DIR *d = opendir(lib);
    if (!d) {
        if (fresh)
            notify("%s: no PS4 games found", label);
        return;
    }
    static title_t titles[MAX_TITLES];
    int count = 0, pending = 0;
    struct dirent *e;
    while ((e = readdir(d)) && count < MAX_TITLES) {
        if (strlen(e->d_name) != 9 || strncmp(e->d_name, "CUSA", 4))
            continue;
        char pkg_path[PATH_MAX];
        snprintf(pkg_path, sizeof(pkg_path), "%s/%s/app.pkg", lib, e->d_name);
        if (!exists(pkg_path))
            continue;
        title_src_t src;
        if (src_open(&src, root, e->d_name))
            continue;
        if (reconnected) {
            char rm[64];
            snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", e->d_name);
            unlink(rm); /* reconnecting the USB drive brings removed games back */
        }
        title_t *t = &titles[count++];
        snprintf(t->tid, sizeof(t->tid), "%s", e->d_name);
        t->state = title_state(&src);
        if (t->state == 1 && retry_waiting(t->tid))
            t->state = -1; /* failed recently: tried again later */
        pending += t->state == 1;
        src_close(&src);
    }
    closedir(d);
    if (fresh && !count)
        notify("%s: no PS4 games found", label);
    else if (fresh)
        notify("%s: %d PS4 game(s) found, %d new or updated", label, count, pending);
    else if (pending)
        notify("%s: %d new or updated PS4 game(s)", label, pending);
    int done = 0, ok = 0;
    for (int i = 0; i < count; i++) {
        title_t *t = &titles[i];
        if (t->state < 0)
            continue;
        title_src_t src;
        if (src_open(&src, root, t->tid))
            continue;
        if (t->state == 0) {
            enforce_patch_sfo(&src);
            manage(&src);
            managed_t *m = managed_by(t->tid, NULL);
            int running = title_running(t->tid);
            if (m && running && !m->running) {
                m->running = 1; /* open before this daemon started (restart or crash) */
            } else if (m && !running && m->running && time(NULL) - m->running_since > 60) {
                m->running = 0; /* closed without a klog end event */
            }
            if (mount_title(&src))
                notify("Failed to mount %s (%s)", src.name, t->tid);
        } else {
            notify("Adding %d/%d: %s%s (%s)", ++done, pending, src.name,
                   src.has_patch ? " with update" : "", t->tid);
            if (register_title(&src)) {
                if (retry_failed(t->tid))
                    notify("Failed to add %s (%s), it is tried again every few minutes", src.name, t->tid);
            } else {
                retry_clear(t->tid);
                ok++;
                notify("%s ready to play", src.name);
            }
        }
        src_close(&src);
    }
    if (pending > 1)
        notify("%s: %d of %d game(s) added", label, ok, pending);
}

/* Releases mounts over /user/app/<TID> of older versions, which mounted the drive
 * folder itself, once that folder is gone. The internal folders mounted now stay
 * while the drive is away (rest mode powers drives off): their pkg links simply
 * point to nothing until the drive is back. */
static void release_stale(void) {
    DIR *d = opendir(STATE_DIR);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n != 13 || strcmp(e->d_name + 9, ".src"))
            continue;
        char tid[10], target[64], from[PATH_MAX], pkg_path[PATH_MAX + 16];
        snprintf(tid, sizeof(tid), "%.9s", e->d_name);
        snprintf(target, sizeof(target), "/user/app/%s", tid);
        if (!mounted_from(target, from, sizeof(from)) || strncmp(from, "/mnt/", 5))
            continue;
        snprintf(pkg_path, sizeof(pkg_path), "%s/app.pkg", from);
        if (!exists(pkg_path)) {
            printf("[scan] releasing %s (source gone)\n", target);
            unmount_all_layers(target, 1);
            managed_t *m = managed_by(tid, NULL);
            if (m)
                m->used = 0;
            snprintf(target, sizeof(target), "/user/patch/%s", tid);
            unmount_all_layers(target, 1);
        }
    }
    closedir(d);
}

static void scan_all(void) {
    static int seen[10], first = 1;
    release_stale();
    char root[32];
    for (int i = 0; i < 10; i++) {
        if (i < 8)
            snprintf(root, sizeof(root), "/mnt/usb%d", i);
        else
            snprintf(root, sizeof(root), "/mnt/ext%d", i - 8);
        int present = is_mountpoint(root);
        if (present)
            scan_root(root, !seen[i] || g_manual_scan, !seen[i] && !first);
        seen[i] = present;
    }
    first = 0;
    g_manual_scan = 0;
}

/* --- klog events --- */

static int klog_connect(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(3232);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s >= 0 && !connect(s, (struct sockaddr *)&a, sizeof(a))) {
        printf("[klog] klogsrv\n");
        return s;
    }
    if (s >= 0)
        close(s);
    int fd = open("/dev/klog", O_RDONLY);
    printf("[klog] /dev/klog fd=%d\n", fd);
    return fd;
}

static const char *after(const char *line, const char *tag) {
    const char *p = strstr(line, tag);
    return p ? p + strlen(tag) : NULL;
}

static void end_delete_window(void) {
    g_deleting = 0;
    g_deleting_close_at = 0;
    for (int i = 0; i < MAX_TITLES; i++)
        if (g_managed[i].used)
            mount_paths(g_managed[i].tid, g_managed[i].has_patch);
    printf("[delete] window closed, game folders mounted again\n");
}

/* Screens of the two delete flows: the home screen (Options > Delete) and
 * Settings > Storage. From the first of them until focus leaves both flows the game
 * folders are unmounted, so a delete removes /user/app/<TID> cleanly (a mount point
 * cannot be removed). Returns 2 for a screen that opens the window, 1 for a screen
 * that keeps it open (menus and dialogs on the way), 0 otherwise. */
static int delete_scene(const char *name) {
    static const char *opens[] = {"AppDeleteConfirmScene", "console storage delete content", "DeleteDialogScreen", NULL};
    static const char *keeps[] = {"AppDeletingScene", "ReactPopupMenu", "Noname", "Render.NPXS40021",
                                  "storage management", NULL};
    for (int i = 0; opens[i]; i++)
        if (strstr(name, opens[i]))
            return 2;
    for (int i = 0; keeps[i]; i++)
        if (strstr(name, keeps[i]))
            return 1;
    return 0;
}

static void start_delete_window(void) {
    g_deleting_since = time(NULL);
    g_deleting_close_at = 0;
    if (g_deleting)
        return;
    g_deleting = 1;
    for (int i = 0; i < MAX_TITLES; i++) {
        managed_t *m = &g_managed[i];
        if (!m->used)
            continue;
        if (m->running || title_running(m->tid))
            continue; /* still open: unmounted when it closes (see the End event) */
        unmount_title(m->tid);
    }
    printf("[delete] delete screen open, game folders unmounted\n");
}

/* Copies klog lines about our titles (and crash/mount errors) into the log. */
static void trace_klog(const char *line) {
    static const char *keys[] = {"CRASH", "Trophy:Register", "_pfs_mount", "mountpfs", "PrepareProcessLaunch",
                                 "preLaunchCheck", "scePlayGoCore", "process_mount", "process_check", "sqlite",
                                 "SQLITE", "corrupt", "Corrupt", "CORRUPT", "dbr", "DBR", NULL};
    for (int i = 0; keys[i]; i++)
        if (strstr(line, keys[i])) {
            printf("[klog] %.300s\n", line);
            return;
        }
    for (int i = 0; i < MAX_TITLES; i++)
        if (g_managed[i].used && strstr(line, g_managed[i].tid) && !strstr(line, "BGLayer") && !strstr(line, "PSM.UI")) {
            printf("[klog] %.300s\n", line);
            return;
        }
}

static void handle_klog_line(const char *line) {
    const char *p;
    char id[40];
    trace_klog(line);
    if ((p = after(line, "OnFocusActiveSceneChanged ["))) {
        /* "[from : type] -> [to : type]": follow the delete screens */
        const char *to = strstr(p, "] -> [");
        if (g_deleting)
            printf("[delete] screen %.200s\n", p);
        if (to) {
            to += 6;
            int kind = delete_scene(to);
            if (kind == 2 || (kind == 1 && g_deleting))
                start_delete_window();
            else if (!kind && g_deleting)
                end_delete_window();
        }
    } else if ((p = after(line, "launchApp("))) {
        snprintf(id, sizeof(id), "%.9s", p);
        managed_t *m = managed_by(id, NULL);
        if (m) {
            m->running = 1;
            m->running_since = time(NULL);
        }
    } else if ((p = after(line, "[PlayGoService][End] end ("))) {
        snprintf(id, sizeof(id), "%.9s", p);
        managed_t *m = managed_by(id, NULL);
        if (m) {
            m->running = 0;
            if (g_deleting) {
                /* closed by a delete: take the folder away before the uninstall runs */
                unmount_title(m->tid);
                printf("[delete] %s closed, game folder unmounted\n", m->tid);
            }
        }
    } else if ((p = after(line, "[PlayGoCore][Uninstall] end ("))) {
        snprintf(id, sizeof(id), "%.36s", p);
        managed_t *m = managed_by(NULL, id);
        if (m) {
            char path[64];
            snprintf(path, sizeof(path), STATE_DIR "/%s.removed", m->tid);
            write_file(path, m->dir, strlen(m->dir), 0666);
            snprintf(path, sizeof(path), STATE_DIR "/%s.src", m->tid);
            unlink(path);
            char hold[PATH_MAX];
            hold_path(hold, sizeof(hold), "app", m->tid);
            rm_tree(hold);
            hold_path(hold, sizeof(hold), "patch", m->tid);
            rm_tree(hold);
            notify("%s removed from the library. The pkg stays on the USB drive.", m->name);
            m->used = 0;
        }
        /* the delete is done: mount the other games again shortly after */
        if (g_deleting)
            g_deleting_close_at = time(NULL) + 3;
    }
}

/* --- web panel --- */

static time_t g_next_scan;
static int g_stop;

void app_rescan(void) {
    g_next_scan = 0;
    g_manual_scan = 1;
}
void app_stop(void) { g_stop = 1; }

static void json_str(buf_t *b, const char *s) {
    buf_put(b, "\"", 1);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            buf_put(b, "\\", 1);
        if ((unsigned char)*s >= 0x20)
            buf_put(b, s, 1);
    }
    buf_put(b, "\"", 1);
}

/* Finds the library folder of tid on any connected storage. */
static int find_root(const char *tid, char *root, size_t n) {
    for (int i = 0; i < 10; i++) {
        char r[32], p[PATH_MAX];
        if (i < 8)
            snprintf(r, sizeof(r), "/mnt/usb%d", i);
        else
            snprintf(r, sizeof(r), "/mnt/ext%d", i - 8);
        snprintf(p, sizeof(p), "%s/ps4/%s/app.pkg", r, tid);
        if (is_mountpoint(r) && exists(p)) {
            snprintf(root, n, "%s", r);
            return 0;
        }
    }
    return -1;
}

void app_games_json(buf_t *out) {
    buf_printf(out, "{\"version\":");
    json_str(out, VERSION);
    buf_printf(out, ",\"games\":[");
    int first = 1;
    for (int i = 0; i < 10; i++) {
        char root[32], lib[PATH_MAX];
        if (i < 8)
            snprintf(root, sizeof(root), "/mnt/usb%d", i);
        else
            snprintf(root, sizeof(root), "/mnt/ext%d", i - 8);
        if (!is_mountpoint(root))
            continue;
        snprintf(lib, sizeof(lib), "%s/ps4", root);
        DIR *d = opendir(lib);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strlen(e->d_name) != 9 || strncmp(e->d_name, "CUSA", 4))
                continue;
            title_src_t t;
            char pkg_path[PATH_MAX];
            snprintf(pkg_path, sizeof(pkg_path), "%s/%s/app.pkg", lib, e->d_name);
            if (!exists(pkg_path) || src_open(&t, root, e->d_name))
                continue;
            const char *tid = e->d_name, *state, *mount = "";
            char target[64], from[PATH_MAX];
            snprintf(target, sizeof(target), "/user/app/%s", tid);
            int mounted = mounted_from(target, from, sizeof(from));
            if (mounted) {
                struct statfs sf;
                statfs(target, &sf);
                mount = sf.f_flags & MNT_RDONLY ? "ro" : "rw";
            }
            managed_t *m = managed_by(tid, NULL);
            if (installed_internally(tid))
                state = "internal";
            else if (served_elsewhere(&t))
                state = "duplicate";
            else if (removed(tid))
                state = "removed";
            else if (m && m->running)
                state = "running";
            else if (mounted && db_is_registered(tid))
                state = "ready";
            else
                state = "pending";
            buf_printf(out, "%s{\"title_id\":\"%s\",\"name\":", first ? "" : ",", tid);
            json_str(out, t.name);
            buf_printf(out, ",\"version\":");
            json_str(out, t.ver);
            buf_printf(out, ",\"has_patch\":%s,\"size\":%llu,\"state\":\"%s\",\"mount\":\"%s\",\"source\":",
                       t.has_patch ? "true" : "false",
                       (unsigned long long)(t.app.pkg_size + (t.has_patch ? t.patch.pkg_size : 0)), state, mount);
            json_str(out, t.dir);
            buf_printf(out, ",\"update_issue\":");
            json_str(out, t.update_issue);
            buf_printf(out, "}");
            first = 0;
            src_close(&t);
        }
        closedir(d);
    }
    buf_printf(out, "]}");
}

int app_icon(const char *tid, buf_t *out) {
    char p[PATH_MAX], root[32];
    snprintf(p, sizeof(p), "/user/appmeta/%s/icon0.png", tid);
    int fd = open(p, O_RDONLY);
    if (fd >= 0) {
        char tmp[65536];
        ssize_t n;
        while ((n = read(fd, tmp, sizeof(tmp))) > 0)
            buf_put(out, tmp, (size_t)n);
        close(fd);
        return out->len ? 0 : -1;
    }
    if (find_root(tid, root, sizeof(root)))
        return -1;
    snprintf(p, sizeof(p), "%s/ps4/%s/app.pkg", root, tid);
    pkg_t pkg;
    if (pkg_open(&pkg, p))
        return -1;
    size_t len;
    uint8_t *png = pkg_entry(&pkg, 0x1200, &len);
    pkg_close(&pkg);
    if (!png)
        return -1;
    buf_put(out, png, len);
    free(png);
    return 0;
}

int app_add(const char *tid, char *msg, size_t cap) {
    char rm[64], root[32];
    if (find_root(tid, root, sizeof(root))) {
        snprintf(msg, cap, "%s is not on any connected USB drive", tid);
        return -1;
    }
    snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", tid);
    unlink(rm);
    g_next_scan = 0;
    snprintf(msg, cap, "Adding %s to the library", tid);
    return 0;
}

int app_remove(const char *tid, char *msg, size_t cap) {
    managed_t *m = managed_by(tid, NULL);
    if ((m && m->running) || title_running(tid)) {
        printf("[web] remove %s refused: game running\n", tid);
        snprintf(msg, cap, "Close the game before removing it");
        return -1;
    }
    unmount_title(tid);
    char a[64], b[64];
    snprintf(a, sizeof(a), "/user/app/%s", tid);
    snprintf(b, sizeof(b), "/user/patch/%s", tid);
    if (mounted_from(a, NULL, 0) || mounted_from(b, NULL, 0)) {
        /* never let the system uninstall run while the USB folder is reachable */
        snprintf(msg, cap, "Could not unmount %s; nothing was removed", tid);
        return -1;
    }
    char path[64];
    snprintf(path, sizeof(path), STATE_DIR "/%s.removed", tid);
    write_file(path, tid, strlen(tid), 0666);
    snprintf(path, sizeof(path), STATE_DIR "/%s.src", tid);
    unlink(path);
    int rc = db_is_registered(tid) ? sceAppInstUtilAppUnInstall(tid) : 0;
    if (m)
        m->used = 0;
    printf("[web] remove %s: uninstall=0x%08x\n", tid, rc);
    snprintf(msg, cap, rc ? "Failed to remove %s (0x%08x)" : "%s removed from the library. The pkg stays on the USB drive.", tid, rc);
    return rc ? -1 : 0;
}

/* --- Media tab shortcut (category 65536 + deeplinkUri, as other PS5 web panels) --- */

#define PANEL_ID "PMNT00001"
static const char k_panel_param[] =
    "{\n"
    "    \"titleId\": \"" PANEL_ID "\",\n"
    "    \"applicationCategoryType\": 65536,\n"
    "    \"deeplinkUri\": \"http://127.0.0.1:8770/\",\n"
    "    \"localizedParameters\": {\n"
    "        \"defaultLanguage\": \"en-US\",\n"
    "        \"en-US\": {\n"
    "            \"titleName\": \"PS4Mount\"\n"
    "        }\n"
    "    }\n"
    "}\n";

static int same_file(const char *path, const void *data, size_t size) {
    struct stat st;
    if (stat(path, &st) || (size_t)st.st_size != size)
        return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    uint8_t *tmp = malloc(size);
    int same = tmp && read(fd, tmp, size) == (ssize_t)size && !memcmp(tmp, data, size);
    free(tmp);
    close(fd);
    return same;
}

/* Installs the shortcut when it is missing or out of date (deleting it from the
 * home screen makes the next start install it again). */
static void install_panel_shortcut(void) {
    const char *param = "/user/app/" PANEL_ID "/sce_sys/param.json", *icon = "/user/app/" PANEL_ID "/sce_sys/icon0.png";
    /* sceAppInstUtilAppExists reports 0 for this shortcut on fw 13.20; app.db is reliable */
    int present = db_is_registered(PANEL_ID), rc = 0;
    int same_param = same_file(param, k_panel_param, sizeof(k_panel_param) - 1);
    int same_icon = same_file(icon, icon0_png, sizeof(icon0_png));
    printf("[panel] exists rc=0x%08x present=%d param=%d icon=%d\n", rc, present, same_param, same_icon);
    if (!rc && present && same_param && same_icon)
        return;
    mkdirs("/user/app/" PANEL_ID "/sce_sys", 0755);
    if (write_file(param, k_panel_param, sizeof(k_panel_param) - 1, 0644) ||
        write_file(icon, icon0_png, sizeof(icon0_png), 0644)) {
        printf("[panel] shortcut files: %s\n", strerror(errno));
        return;
    }
    rc = sceAppInstUtilAppInstallAll(NULL);
    printf("[panel] shortcut install rc=0x%08x\n", rc);
    if (!rc)
        notify("PS4Mount added to the Media tab");
}

static int single_instance(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(LOCK_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 1)) {
        if (s >= 0)
            close(s);
        return -1;
    }
    return s;
}

/* Rest mode closes the network sockets of payloads: a listening socket that stopped
 * listening is found here so it can be opened again. */
static int still_listening(int s) {
    int on = 0;
    socklen_t n = sizeof(on);
    return s >= 0 && !getsockopt(s, SOL_SOCKET, SO_ACCEPTCONN, &on, &n) && on;
}

static int g_klog = -1, g_web = -1;
static int g_live; /* klogsrv replays its backlog first: live after the first quiet 100 ms */

/* Serves the panel and handles klog lines for up to ms milliseconds. */
static void pump_events(int ms) {
    static char buf[16384], line[4096];
    static size_t ll;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    end.tv_sec += ms / 1000;
    end.tv_nsec += (long)(ms % 1000) * 1000000;
    for (;;) {
        if (g_klog < 0) {
            g_klog = klog_connect();
            g_live = 0;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long left = (end.tv_sec - now.tv_sec) * 1000 + (end.tv_nsec - now.tv_nsec) / 1000000;
        if (left <= 0)
            return;
        long wait = g_live || g_klog < 0 ? left : (left < 100 ? left : 100);
        struct timeval tv = {wait / 1000, (wait % 1000) * 1000};
        fd_set fs;
        FD_ZERO(&fs);
        int maxfd = -1;
        if (g_klog >= 0) {
            FD_SET(g_klog, &fs);
            maxfd = g_klog;
        }
        if (g_web >= 0) {
            FD_SET(g_web, &fs);
            if (g_web > maxfd)
                maxfd = g_web;
        }
        if (maxfd < 0) {
            usleep((useconds_t)left * 1000);
            return;
        }
        int ready = select(maxfd + 1, &fs, NULL, NULL, &tv);
        if (ready == 0 && !g_live && g_klog >= 0) {
            g_live = 1;
            printf("[klog] backlog done, following live\n");
        }
        if (ready <= 0)
            continue;
        if (g_web >= 0 && FD_ISSET(g_web, &fs))
            web_serve(g_web);
        if (g_klog < 0 || !FD_ISSET(g_klog, &fs))
            continue;
        ssize_t n = read(g_klog, buf, sizeof(buf));
        if (n <= 0) {
            close(g_klog);
            g_klog = -1;
            continue;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] != '\n') {
                if (ll < sizeof(line) - 1)
                    line[ll++] = buf[i];
                continue;
            }
            line[ll] = 0;
            ll = 0;
            if (g_live)
                handle_klog_line(line);
        }
    }
}

int main(void) {
    int lock = single_instance();
    if (lock < 0) {
        printf(VERSION " already running\n");
        return 0;
    }
    sceUserServiceInitialize(0);
    sceAppInstUtilInitialize();
    mkdirs(STATE_DIR, 0777);
    freopen(STATE_DIR "/log.txt", "a", stdout);
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    install_panel_shortcut();
    g_web = web_listen();
    notify(g_web >= 0 ? VERSION " running. Panel in the Media tab or http://<PS5 IP>:8770" : VERSION " running (panel unavailable)");
    unlink(STATE_DIR "/stop");
    for (;;) {
        if (g_stop || exists(STATE_DIR "/stop")) {
            unlink(STATE_DIR "/stop");
            notify(VERSION " stopped");
            return 0;
        }
        if (!still_listening(lock)) {
            /* lost in rest mode: take it again, or stop if another PS4Mount has it now */
            close(lock);
            if ((lock = single_instance()) < 0) {
                notify(VERSION " stopped: another PS4Mount is running");
                return 0;
            }
            printf("[main] instance lock opened again\n");
        }
        if (!still_listening(g_web)) {
            int was = g_web >= 0;
            if (g_web >= 0)
                close(g_web);
            g_web = web_listen();
            if (was || g_web >= 0) /* log changes only, not every retry */
                printf("[main] web panel %s\n", g_web >= 0 ? "listening again" : "unavailable");
        }
        if (g_deleting && g_deleting_close_at && time(NULL) >= g_deleting_close_at)
            end_delete_window();
        if (g_deleting && time(NULL) - g_deleting_since > 600)
            end_delete_window(); /* the line of the screen change was missed */
        if (time(NULL) >= g_next_scan && !g_deleting) {
            scan_all();
            g_next_scan = time(NULL) + SCAN_SECONDS;
        }
        pump_events(1000);
    }
}
