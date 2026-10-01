/*
 * File manager over HTTP for the Wi-Fi access point (see webfs.h).
 *
 * esp_http_server runs every request on its one task (stack in PSRAM), so
 * a single internal, DMA-capable buffer serves all transfers: the SDMMC
 * driver would bounce PSRAM buffers sector by sector. Uploads fill it
 * completely before each write so the card sees whole clusters.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_vfs_fat.h"
#include "esp_timer.h"

#include "sdcard.h"
#include "tracklog.h"
#include "wifi_ap.h"
#include "webfs.h"

static const char *TAG = "webfs";

#define BUF_SIZE  8192
#define PATH_MAX_ 300

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_server;
static char *s_buf;

/* ---- paths ------------------------------------------------------------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* percent-decode src[0..len) (len < 0: up to '\0' / '?'), '+' stays '+' */
static bool url_decode(char *dst, size_t n, const char *src, int len)
{
    size_t o = 0;
    for (int i = 0; (len < 0 ? src[i] && src[i] != '?' : i < len); i++) {
        char c = src[i];
        if (c == '%') {
            int h = hexval(src[i + 1]), l = h < 0 ? -1 : hexval(src[i + 2]);
            if (h < 0 || l < 0) return false;
            c = (char)(h << 4 | l);
            i += 2;
        }
        if (o + 1 >= n) return false;
        dst[o++] = c;
    }
    dst[o] = 0;
    return true;
}

/* card-relative "/a/b" -> "/sdcard/a/b"; refuses "..", "\" and control characters */
static bool make_path(char *full, size_t n, const char *rel)
{
    if (rel[0] != '/') return false;
    for (const char *p = rel; *p; p++) {
        if ((unsigned char)*p < 0x20 || *p == '\\') return false;
        if (p[0] == '.' && p[1] == '.' && (p[-1] == '/') && (p[2] == '/' || p[2] == 0)) return false;
    }
    int w = snprintf(full, n, SD_MOUNT_POINT "%s", rel);
    if (w < 0 || (size_t)w >= n) return false;
    /* no trailing slash except for the root itself */
    while (w > (int)sizeof SD_MOUNT_POINT && full[w - 1] == '/') full[--w] = 0;
    if (full[w - 1] == '/') full[w - 1] = 0;   /* "/sdcard/" -> "/sdcard" */
    return true;
}

static bool path_from_uri(httpd_req_t *req, char *full, size_t n)
{
    char rel[PATH_MAX_];
    return url_decode(rel, sizeof rel, req->uri + 3, -1) && make_path(full, n, rel);   /* skip "/fs" */
}

static bool path_from_query(httpd_req_t *req, const char *key, char *full, size_t n)
{
    char q[PATH_MAX_ * 3 * 2], v[PATH_MAX_ * 3], rel[PATH_MAX_];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, v, sizeof v) != ESP_OK) return false;
    return url_decode(rel, sizeof rel, v, strlen(v)) && make_path(full, n, rel);
}

static bool is_recording(const char *full)
{
    return tracklog_active() && strcmp(full, tracklog_filename()) == 0;
}

/* httpd_send() may return after a partial write */
static bool send_all(httpd_req_t *req, const char *p, int n)
{
    while (n > 0) {
        int w = httpd_send(req, p, n);
        if (w <= 0) return false;
        p += w;
        n -= w;
    }
    return true;
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *msg)
{
    ESP_LOGW(TAG, "%s %s: %s", req->method == HTTP_GET ? "GET" : req->method == HTTP_PUT ? "PUT" : "req",
             req->uri, msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

/* ---- handlers ---------------------------------------------------------- */

static esp_err_t index_get(httpd_req_t *req)
{
    wifi_ap_touch();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);  /* EMBED_TXTFILES adds a NUL */
}

/* JSON string body (no quotes) */
static int json_esc(char *dst, size_t n, const char *s)
{
    size_t o = 0;
    for (; *s && o + 7 < n; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = c; }
        else if (c < 0x20) o += snprintf(dst + o, n - o, "\\u%04x", c);
        else dst[o++] = c;
    }
    dst[o] = 0;
    return o;
}

static esp_err_t ls_get(httpd_req_t *req)
{
    wifi_ap_touch();
    char dir[PATH_MAX_], full[PATH_MAX_ + 260], name[260 * 2];
    if (!path_from_query(req, "d", dir, sizeof dir)) return fail(req, "400 Bad Request", "bad path");
    DIR *d = opendir(dir);
    if (!d) return fail(req, "404 Not Found", "no such directory");

    uint64_t total = 0, freeb = 0;
    esp_vfs_fat_info(SD_MOUNT_POINT, &total, &freeb);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    size_t o = snprintf(s_buf, BUF_SIZE, "{\"total\":%llu,\"free\":%llu,\"entries\":[",
                        (unsigned long long)total, (unsigned long long)freeb);
    bool first = true;
    esp_err_t err = ESP_OK;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && err == ESP_OK) {
        struct stat st = {0};
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        stat(full, &st);
        json_esc(name, sizeof name, e->d_name);
        o += snprintf(s_buf + o, BUF_SIZE - o, "%s{\"n\":\"%s\",\"d\":%d,\"s\":%lu,\"t\":%lld}",
                      first ? "" : ",", name, e->d_type == DT_DIR, (unsigned long)st.st_size, (long long)st.st_mtime);
        first = false;
        if (o > BUF_SIZE - 1024) {
            err = httpd_resp_send_chunk(req, s_buf, o);
            o = 0;
        }
    }
    closedir(d);
    if (err != ESP_OK) return err;
    o += snprintf(s_buf + o, BUF_SIZE - o, "]}");
    httpd_resp_send_chunk(req, s_buf, o);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static const char *mime(const char *p)
{
    const char *x = strrchr(p, '.');
    if (!x) return "application/octet-stream";
    if (!strcasecmp(x, ".txt") || !strcasecmp(x, ".log") || !strcasecmp(x, ".csv")) return "text/plain; charset=utf-8";
    if (!strcasecmp(x, ".json") || !strcasecmp(x, ".config")) return "application/json";
    if (!strcasecmp(x, ".gpx") || !strcasecmp(x, ".xml")) return "application/xml";
    if (!strcasecmp(x, ".html") || !strcasecmp(x, ".htm")) return "text/plain; charset=utf-8";   /* never render card content as a page */
    if (!strcasecmp(x, ".png")) return "image/png";
    if (!strcasecmp(x, ".jpg") || !strcasecmp(x, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(x, ".bmp")) return "image/bmp";
    return "application/octet-stream";
}

static esp_err_t file_get(httpd_req_t *req)
{
    wifi_ap_touch();
    char path[PATH_MAX_];
    if (!path_from_uri(req, path, sizeof path)) return fail(req, "400 Bad Request", "bad path");
    int fd = open(path, O_RDONLY);
    if (fd < 0) return errno == EACCES ? fail(req, "409 Conflict", "file is being written")
                                       : fail(req, "404 Not Found", "no such file");
    struct stat st;
    fstat(fd, &st);

    /* headers by hand: esp_http_server only streams with chunked encoding,
     * and a Content-Length lets the browser show progress */
    char q[16], v[4];
    const char *base = strrchr(path, '/') + 1;
    int o = snprintf(s_buf, BUF_SIZE, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n",
                     mime(path), (long)st.st_size);
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK && httpd_query_key_value(q, "dl", v, sizeof v) == ESP_OK) {
        /* RFC 6266 filename*: percent-encode everything that is not plain */
        o += snprintf(s_buf + o, BUF_SIZE - o, "Content-Disposition: attachment; filename*=UTF-8''");
        for (const unsigned char *p = (const unsigned char *)base; *p && o < 2048; p++) {
            if (isalnum(*p) || strchr("-._~", *p)) s_buf[o++] = *p;
            else o += snprintf(s_buf + o, BUF_SIZE - o, "%%%02X", *p);
        }
        o += snprintf(s_buf + o, BUF_SIZE - o, "\r\n");
    }
    o += snprintf(s_buf + o, BUF_SIZE - o, "\r\n");
    esp_err_t err = send_all(req, s_buf, o) ? ESP_OK : ESP_FAIL;
    int64_t t0 = esp_timer_get_time();
    while (err == ESP_OK) {
        ssize_t n = read(fd, s_buf, BUF_SIZE);
        if (n <= 0) { err = n < 0 ? ESP_FAIL : ESP_OK; break; }
        wifi_ap_touch();
        if (!send_all(req, s_buf, n)) err = ESP_FAIL;
    }
    close(fd);
    int64_t dt = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "GET %s %ld B in %lld ms", path, (long)st.st_size, dt / 1000);
    return err;
}

static esp_err_t file_put(httpd_req_t *req)
{
    wifi_ap_touch();
    char path[PATH_MAX_], part[PATH_MAX_ + 8];
    if (!path_from_uri(req, path, sizeof path) || strcmp(path, SD_MOUNT_POINT) == 0) return fail(req, "400 Bad Request", "bad path");
    if (is_recording(path)) return fail(req, "409 Conflict", "file is being recorded");
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return fail(req, "409 Conflict", "is a directory");
    snprintf(part, sizeof part, "%s.part", path);
    int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fail(req, "500 Internal Server Error", "cannot create file (does the folder exist?)");

    int64_t t0 = esp_timer_get_time();
    size_t left = req->content_len;
    int timeouts = 0;
    size_t fill = 0;
    while (left > 0 || fill > 0) {
        if (left > 0 && fill < BUF_SIZE) {
            int n = httpd_req_recv(req, s_buf + fill, left < BUF_SIZE - fill ? left : BUF_SIZE - fill);
            if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
            if (n <= 0) {
                close(fd);
                unlink(part);
                ESP_LOGW(TAG, "PUT %s aborted, %u B left", path, (unsigned)left);
                return ESP_FAIL;   /* closes the connection */
            }
            timeouts = 0;
            fill += n;
            left -= n;
            if (fill < BUF_SIZE && left > 0) continue;
        }
        wifi_ap_touch();
        if (write(fd, s_buf, fill) != (ssize_t)fill) {
            close(fd);
            unlink(part);
            return fail(req, "507 Insufficient Storage", "write failed (card full?)");
        }
        fill = 0;
    }
    close(fd);
    /* FAT rename does not replace; FS_LOCK refuses (EACCES) an open file */
    if (unlink(path) != 0 && errno != ENOENT) {
        unlink(part);
        return fail(req, "409 Conflict", errno == EACCES ? "file is in use (open map?)" : "cannot replace the file");
    }
    if (rename(part, path) != 0) {
        unlink(part);
        return fail(req, "500 Internal Server Error", "rename failed");
    }
    int64_t dt = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "PUT %s %u B in %lld ms (%lld KB/s)", path, (unsigned)req->content_len, dt / 1000,
             dt > 0 ? (long long)req->content_len * 1000 / dt : 0);
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t file_delete(httpd_req_t *req)
{
    wifi_ap_touch();
    char path[PATH_MAX_];
    if (!path_from_uri(req, path, sizeof path) || strcmp(path, SD_MOUNT_POINT) == 0) return fail(req, "400 Bad Request", "bad path");
    if (is_recording(path)) return fail(req, "409 Conflict", "file is being recorded");
    struct stat st;
    if (stat(path, &st) != 0) return fail(req, "404 Not Found", "no such file");
    if (S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path)) {
        return fail(req, "409 Conflict", errno == EACCES ? "file is in use (open map?)" :
                                         S_ISDIR(st.st_mode) ? "folder is not empty" : "delete failed");
    }
    ESP_LOGI(TAG, "DELETE %s", path);
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t mkdir_post(httpd_req_t *req)
{
    wifi_ap_touch();
    char path[PATH_MAX_];
    if (!path_from_query(req, "d", path, sizeof path)) return fail(req, "400 Bad Request", "bad path");
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return fail(req, "500 Internal Server Error", "mkdir failed");
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t mv_post(httpd_req_t *req)
{
    wifi_ap_touch();
    char from[PATH_MAX_], to[PATH_MAX_];
    if (!path_from_query(req, "from", from, sizeof from) || !path_from_query(req, "to", to, sizeof to))
        return fail(req, "400 Bad Request", "bad path");
    if (is_recording(from)) return fail(req, "409 Conflict", "file is being recorded");
    struct stat st;
    if (stat(to, &st) == 0) return fail(req, "409 Conflict", "target exists");
    if (rename(from, to) != 0) return fail(req, errno == EACCES ? "409 Conflict" : "500 Internal Server Error",
                                           errno == EACCES ? "file is in use (open map?)" : "rename failed");
    ESP_LOGI(TAG, "MV %s -> %s", from, to);
    return httpd_resp_sendstr(req, "ok");
}

/* ---- server ------------------------------------------------------------ */

esp_err_t webfs_start(void)
{
    if (s_server) return ESP_OK;
    s_buf = heap_caps_malloc(BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_buf) s_buf = malloc(BUF_SIZE);
    if (!s_buf) return ESP_ERR_NO_MEM;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    cfg.max_uri_handlers = 8;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 20;
    cfg.send_wait_timeout = 20;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {   /* no PSRAM: internal stack */
        cfg.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        err = httpd_start(&s_server, &cfg);
    }
    if (err != ESP_OK) {
        free(s_buf);
        s_buf = NULL;
        s_server = NULL;
        return err;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/",          .method = HTTP_GET,    .handler = index_get },
        { .uri = "/api/ls",    .method = HTTP_GET,    .handler = ls_get },
        { .uri = "/api/mkdir", .method = HTTP_POST,   .handler = mkdir_post },
        { .uri = "/api/mv",    .method = HTTP_POST,   .handler = mv_post },
        { .uri = "/fs/*",      .method = HTTP_GET,    .handler = file_get },
        { .uri = "/fs/*",      .method = HTTP_PUT,    .handler = file_put },
        { .uri = "/fs/*",      .method = HTTP_DELETE, .handler = file_delete },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(s_server, &uris[i]);
    ESP_LOGI(TAG, "web server up");
    return ESP_OK;
}

void webfs_stop(void)
{
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = NULL;
    free(s_buf);
    s_buf = NULL;
}
