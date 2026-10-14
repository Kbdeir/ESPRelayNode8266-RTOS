"""Run the production static-page handler and recovery seeder with host mocks.

Uses cc on POSIX or the default WSL distribution on Windows.
"""

import gzip
import os
from pathlib import Path
import random
import subprocess
import tempfile


PROJECT_DIR = Path(__file__).resolve().parent.parent

HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define SC_LFS_NAME_MAX 64
#define ESP_OK 0
#define ESP_FAIL -1
#define HTTPD_404 "404 Not Found"
typedef int esp_err_t;
typedef struct {
    const char *uri;
    const char *type, *encoding, *cache;
    int status;
    unsigned char body[8192];
    size_t length;
    bool finished, fail_send;
} httpd_req_t;
typedef struct {
    const char *path;
    const unsigned char *data;
    size_t length;
} file_t;
static file_t files[4];
static size_t file_count;
static int writes;
static bool fail_read;
static const char *reader;
static size_t reader_offset;
static int closes;

static void reset(void) { file_count = 0; writes = 0; fail_read = false; }
static void put(const char *path, const void *data, size_t length) {
    assert(file_count < 4);
    files[file_count++] = (file_t){path, data, length};
}
static int sc_webfs_read_range(const char *path, size_t off, void *buf,
                               size_t cap, size_t *out) {
    if (fail_read && off) return -1;
    for (size_t i = 0; i < file_count; i++) {
        if (strcmp(path, files[i].path)) continue;
        *out = off < files[i].length ? files[i].length - off : 0;
        if (*out > cap) *out = cap;
        if (*out) memcpy(buf, files[i].data + off, *out);
        return 0;
    }
    return -1;
}
static int sc_lfs_read_range(const char *p, size_t o, void *b, size_t c,
                             size_t *n) {
    return sc_webfs_read_range(p, o, b, c, n);
}
static int sc_lfs_exists(const char *path) {
    for (size_t i = 0; i < file_count; i++)
        if (!strcmp(path, files[i].path)) return 1;
    return 0;
}
static int sc_webfs_read_stream_open(const char *path) {
    assert(!reader);
    if (!sc_lfs_exists(path)) return -1;
    reader = path; reader_offset = 0;
    return 0;
}
static int sc_webfs_read_stream_read(void *data, size_t cap, size_t *out) {
    assert(reader && cap == 256);
    int result = sc_webfs_read_range(reader, reader_offset, data, cap, out);
    if (!result) reader_offset += *out;
    return result;
}
static int sc_webfs_read_stream_close(void) {
    assert(reader); reader = NULL; closes++;
    return 0;
}
static int sc_lfs_write_file(const char *path, const void *data, size_t length) {
    (void)path; (void)data; (void)length;
    writes++;
    return 0;
}
static esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) {
    r->type = type; return ESP_OK;
}
static esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *key,
                                   const char *value) {
    if (!strcmp(key, "Content-Encoding")) r->encoding = value;
    if (!strcmp(key, "Cache-Control")) r->cache = value;
    return ESP_OK;
}
static esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *data,
                                      size_t length) {
    if (r->fail_send) return ESP_FAIL;
    assert(r->length + length <= sizeof(r->body));
    if (length) memcpy(r->body + r->length, data, length);
    else r->finished = true;
    r->length += length;
    return ESP_OK;
}
static esp_err_t send_error(httpd_req_t *r, const char *status, const char *msg) {
    r->status = !strcmp(status, HTTPD_404) ? 404 : 500;
    return httpd_resp_send_chunk(r, msg, strlen(msg));
}
'''

CHECKS = r'''
static void expect_page(const char *uri, const char *type,
                        const unsigned char *data, size_t length, bool gzip) {
    httpd_req_t r = {.uri = uri};
    assert(static_file(&r) == ESP_OK);
    assert(r.status == 0 && r.finished && r.length == length);
    assert(!memcmp(r.body, data, length));
    assert(r.type && !strcmp(r.type, type));
    assert(r.cache && !strcmp(r.cache, "no-store"));
    assert(gzip ? r.encoding && !strcmp(r.encoding, "gzip") : !r.encoding);
    assert(!reader);
}
int main(void) {
    const unsigned char plain[] = "<html>uploaded page</html>";
    reset();
    // Reproduce the multi-upload result: config.html removed, .gz remains.
    put("/config.html.gz", compressed_data, sizeof(compressed_data));
    expect_page("/config.html", "text/html", compressed_data,
                sizeof(compressed_data), true);
    expect_page("/config.html?refresh=1", "text/html", compressed_data,
                sizeof(compressed_data), true);
    seed("/config.html", "compiled recovery");
    assert(writes == 0 && !sc_lfs_exists("/config.html"));
    expect_page("/config.html", "text/html", compressed_data,
                sizeof(compressed_data), true);
    put("/config.html", plain, sizeof(plain) - 1);
    expect_page("/config.html", "text/html", plain, sizeof(plain) - 1, false);

    const char *paths[] = {"/index.html.gz", "/Input_Relays_Map.html.gz",
                           "/Automation.html.gz", "/site.css.gz", "/site.js.gz"};
    const char *uris[] = {"/", "/Input_Relays_Map", "/Automation",
                          "/site.css", "/site.js"};
    const char *types[] = {"text/html", "text/html", "text/html", "text/css",
                           "application/javascript"};
    for (size_t i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {
        reset();
        put(paths[i], compressed_data, sizeof(compressed_data));
        expect_page(uris[i], types[i], compressed_data,
                    sizeof(compressed_data), true);
    }

    reset();
    put("/config.html", plain, 0);
    expect_page("/config.html", "text/html", plain, 0, false);
    unsigned char boundary[768] = {0};
    reset();
    put("/config.html", boundary, sizeof(boundary));
    expect_page("/config.html", "text/html", boundary, sizeof(boundary), false);

    reset();
    httpd_req_t missing = {.uri = "/config.html"};
    static_file(&missing);
    assert(missing.status == 404 && !missing.encoding);
    char long_path[80];
    memset(long_path, 'a', sizeof(long_path));
    long_path[0] = '/'; long_path[sizeof(long_path) - 1] = 0;
    httpd_req_t too_long = {.uri = long_path};
    static_file(&too_long);
    assert(too_long.status == 404);
    long_path[SC_LFS_NAME_MAX - 1] = 0;
    httpd_req_t suffix_overflow = {.uri = long_path};
    static_file(&suffix_overflow);
    assert(suffix_overflow.status == 404);

    seed("/config.html", "compiled recovery");
    assert(writes == 1);
    reset();
    put("/config.html", plain, sizeof(plain) - 1);
    seed("/config.html", (const char *)plain);
    assert(writes == 0);
    seed("/config.html", "new compiled recovery");
    assert(writes == 1);

    reset();
    put("/config.html.gz", compressed_data, sizeof(compressed_data));
    httpd_req_t failed_send = {.uri = "/config.html", .fail_send = true};
    assert(static_file(&failed_send) == ESP_FAIL && !failed_send.finished);
    assert(!reader);
    fail_read = true;
    httpd_req_t failed_read = {.uri = "/config.html"};
    assert(static_file(&failed_read) == ESP_FAIL && !failed_read.finished);
    assert(!reader && closes > 0);
    puts("PASS: gzip uploads, reboot recovery, raw precedence, aliases, MIME, "
         "chunk boundaries, missing files, path limits and stream failures");
    return 0;
}
'''


def main():
    web = (PROJECT_DIR / "components/sc_web/sc_web.c").read_text(encoding="utf-8")
    webfs = (PROJECT_DIR / "components/sc_webfs/sc_webfs.c").read_text(encoding="utf-8")
    handler = web[web.index("static esp_err_t static_file("):web.index("\nesp_err_t sc_web_init(")]
    seeder = webfs[webfs.index("static void seed("):webfs.index("esp_err_t sc_webfs_init(")]
    rng = random.Random(0)
    html = b"<html>" + bytes(rng.choice(b"abcdefghijklmnopqrstuvwxyz") for _ in range(4000)) + b"</html>"
    compressed = gzip.compress(html, mtime=0)
    assert len(compressed) > 768 and gzip.decompress(compressed) == html
    data = "static const unsigned char compressed_data[] = {" + ",".join(map(str, compressed)) + "};\n"
    with tempfile.TemporaryDirectory(prefix="web-gzip-") as temp:
        work = Path(temp)
        (work / "test.c").write_text(HARNESS + data + handler + seeder + CHECKS, encoding="utf-8")
        prefix = ["wsl", "--cd", str(work), "--exec"] if os.name == "nt" else []
        subprocess.run(prefix + ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
                                 "test.c", "-o", "test"], cwd=work, check=True)
        subprocess.run(prefix + ["./test"], cwd=work, check=True)


if __name__ == "__main__":
    main()
