#include "web.h"
#include "metrics.h"
#include "commission.h"
#include <jansson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>

static char *dup_str(const char *s, size_t *len)
{
    *len = strlen(s);
    char *p = malloc(*len + 1);
    if (p) memcpy(p, s, *len + 1);
    return p;
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 4 << 20) { fclose(f); return NULL; }
    char *b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[n] = 0; *len = (size_t)n; }
    return b;
}

static int has_query_key(const char *uri, const char *key)
{
    const char *q = uri ? strchr(uri, '?') : NULL;
    size_t kl = strlen(key);
    while (q) {
        q++;
        if (!strncmp(q, key, kl) && (q[kl] == 0 || q[kl] == '&' || q[kl] == '=')) return 1;
        q = strchr(q, '&');
    }
    return 0;
}

static web_reply json_reply(int status, json_t *o)
{
    web_reply r = { status, "application/json", NULL, 0 };
    r.body = json_dumps(o, JSON_COMPACT);
    json_decref(o);
    r.len = r.body ? strlen(r.body) : 0;
    return r;
}

static web_reply json_error(int status, const char *msg)
{
    return json_reply(status, json_pack("{s:{s:s}}", "error", "message", msg));
}

static json_t *commission_view(const web_commissioning *wc)
{
    commission c;
    int have = commission_load(wc->state_dir, &c) == 0;
    char st[256] = "";
    if (wc->status) wc->status(st, sizeof st, wc->user);
    json_t *o = json_pack("{s:b,s:s,s:s,s:b,s:o,s:s}", "commissioned", have, "url", have ? c.url : "",
                          "deviceId", have ? c.device_id : "", "tokenSet", have && c.token[0],
                          "cert", commission_cert_summary(have ? c.trust : ""), "status", st);
    explicit_bzero(&c, sizeof c);
    return o;
}

static web_reply commission_post(const web_request *rq, const web_commissioning *wc)
{
    if (!rq->csrf_header) return json_error(403, "missing X-AURIX-Request header");
    if (!rq->body || rq->body_len == 0 || rq->body_len > 65536) return json_error(400, "request body missing or too large");
    json_t *in = json_loadb(rq->body, rq->body_len, 0, NULL);
    if (!json_is_object(in)) { json_decref(in); return json_error(400, "body must be a JSON object"); }

    if (json_is_true(json_object_get(in, "clear"))) {
        json_decref(in);
        commission_clear(wc->state_dir);
        if (wc->changed) wc->changed(wc->user);
        return json_reply(200, commission_view(wc));
    }
    commission old, c;
    int had = commission_load(wc->state_dir, &old) == 0;
    memset(&c, 0, sizeof c);
    const char *u = json_string_value(json_object_get(in, "url"));
    const char *d = json_string_value(json_object_get(in, "deviceId"));
    const char *t = json_string_value(json_object_get(in, "token"));
    json_t *cj = json_object_get(in, "cert");
    snprintf(c.url, sizeof c.url, "%s", u ? u : "");
    snprintf(c.device_id, sizeof c.device_id, "%s", d ? d : "");
    /* blank token / absent cert = keep what is saved */
    snprintf(c.token, sizeof c.token, "%s", t && *t ? t : had ? old.token : "");
    if (json_is_string(cj)) {
        if (json_string_length(cj) >= sizeof c.trust) { json_decref(in); explicit_bzero(&old, sizeof old); return json_error(400, "certificate too large"); }
        snprintf(c.trust, sizeof c.trust, "%s", json_string_value(cj));
    } else if (had) {
        snprintf(c.trust, sizeof c.trust, "%s", old.trust);
    }
    json_decref(in);
    explicit_bzero(&old, sizeof old);
    /* strip surrounding whitespace the browser may add */
    for (char *f = c.url + strlen(c.url); f > c.url && (f[-1] == ' ' || f[-1] == '\n' || f[-1] == '\r'); ) *--f = 0;
    char why[256] = "";
    if (commission_validate(&c, why, sizeof why)) { explicit_bzero(&c, sizeof c); return json_error(422, why); }
    int rc = commission_save(wc->state_dir, &c);
    explicit_bzero(&c, sizeof c);
    if (rc) return json_error(500, "could not save on the camera (storage full?)");
    syslog(LOG_INFO, "web: Pharos connection saved from the AURIX page");
    if (wc->changed) wc->changed(wc->user);
    return json_reply(200, commission_view(wc));
}

web_reply web_route(const web_request *rq, const char *html_path, const web_commissioning *wc)
{
    web_reply r = { 200, "text/html; charset=utf-8", NULL, 0 };
    int post = rq->method && !strcmp(rq->method, "POST");
    if (has_query_key(rq->uri, "commission")) {
        if (!wc || !wc->state_dir) return json_error(503, "commissioning unavailable");
        return post ? commission_post(rq, wc) : json_reply(200, commission_view(wc));
    }
    if (has_query_key(rq->uri, "settings")) {
        if (!wc || !wc->settings_get) return json_error(503, "settings unavailable");
        if (post) {
            if (!rq->csrf_header) return json_error(403, "missing X-AURIX-Request header");
            json_t *in = rq->body && rq->body_len <= 65536 ? json_loadb(rq->body, rq->body_len, 0, NULL) : NULL;
            char why[200] = "";
            int rc = in && wc->settings_set ? wc->settings_set(in, why, sizeof why, wc->user) : -1;
            json_decref(in);
            if (rc) return json_error(422, why[0] ? why : "body must be a JSON object of settings");
            syslog(LOG_INFO, "web: settings saved from the AURIX page");
        }
        return json_reply(200, wc->settings_get(wc->user));
    }
    if (post) return json_error(405, "POST is only accepted for ?commission and ?settings");
    if (has_query_key(rq->uri, "data")) {
        json_t *m = metrics_json();
        r.body = json_dumps(m, JSON_COMPACT);
        json_decref(m);
        r.content_type = "application/json";
        r.len = r.body ? strlen(r.body) : 0;
        if (!r.body) { r.status = 500; r.body = dup_str("{\"error\":\"metrics\"}", &r.len); }
        return r;
    }
    r.body = read_file(html_path, &r.len);
    if (!r.body) {
        r.status = 500;
        r.content_type = "text/plain; charset=utf-8";
        r.body = dup_str("AURIX dashboard file is missing from the package (web/dashboard.html).\n", &r.len);
    }
    return r;
}

#ifndef AURIX_NO_FCGI
#include <fcgiapp.h>

static char g_html[256];
static web_commissioning g_wc;

static void *fcgi_loop(void *arg)
{
    int sock = (int)(long)arg;
    FCGX_Request req;
    if (FCGX_InitRequest(&req, sock, 0)) { syslog(LOG_ERR, "web: FCGX_InitRequest failed"); return NULL; }
    while (FCGX_Accept_r(&req) == 0) {
        const char *method = FCGX_GetParam("REQUEST_METHOD", req.envp);
        const char *cl = FCGX_GetParam("CONTENT_LENGTH", req.envp);
        const char *xr = FCGX_GetParam("HTTP_X_AURIX_REQUEST", req.envp);
        long want = cl ? atol(cl) : 0;
        char *body = NULL;
        int got = 0;
        if (want > 0 && want <= 65536) {
            body = malloc((size_t)want + 1);
            if (body) { got = FCGX_GetStr(body, (int)want, req.in); body[got > 0 ? got : 0] = 0; }
        }
        web_request rq = { method, FCGX_GetParam("REQUEST_URI", req.envp), body, got > 0 ? (size_t)got : 0,
                           xr && !strcmp(xr, "1") };
        web_reply r = web_route(&rq, g_html, &g_wc);
        static unsigned served;
        if (++served <= 3 || r.status >= 400)
            syslog(r.status >= 500 ? LOG_ERR : LOG_INFO, "web: %s %s -> %d", method ? method : "?",
                   rq.uri ? rq.uri : "?", r.status);
        if (body) { explicit_bzero(body, (size_t)want); free(body); }
        FCGX_FPrintF(req.out, "Status: %d\r\nContent-Type: %s\r\nCache-Control: no-store\r\n"
                              "X-Content-Type-Options: nosniff\r\nContent-Length: %lu\r\n\r\n",  /* libfcgi printf has no %zu */
                     r.status, r.content_type, (unsigned long)r.len);
        if (r.body) FCGX_PutStr(r.body, (int)r.len, req.out);
        free(r.body);
        FCGX_Finish_r(&req);
    }
    return NULL;
}

int web_start(const char *html_path, const web_commissioning *wc)
{
    if (wc) g_wc = *wc;
    const char *path = getenv("FCGI_SOCKET_NAME");
    if (!path) {
        syslog(LOG_WARNING, "web: FCGI_SOCKET_NAME not set - dashboard unavailable");
        return -1;
    }
    snprintf(g_html, sizeof g_html, "%s", html_path);
    if (FCGX_Init()) { syslog(LOG_ERR, "web: FCGX_Init failed"); return -1; }
    int sock = FCGX_OpenSocket(path, 5);
    if (sock < 0) { syslog(LOG_ERR, "web: cannot open %s", path); return -1; }
    /* The camera's web server runs as another user: it must be allowed to connect, or every
     * request is answered 503 (same as Axis' FastCGI examples). */
    if (chmod(path, S_IRWXU | S_IRWXG | S_IRWXO) != 0)
        syslog(LOG_ERR, "web: cannot set permissions on %s - the page will answer 503", path);
    pthread_t th;
    if (pthread_create(&th, NULL, fcgi_loop, (void *)(long)sock)) return -1;
    pthread_detach(th);
    syslog(LOG_INFO, "web: dashboard at /local/aurix/aurix.cgi (socket %s)", path);
    return 0;
}
#endif
