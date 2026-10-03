#include "web.h"
#include "metrics.h"
#include <jansson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

web_reply web_route(const char *uri, const char *html_path)
{
    web_reply r = { 200, "text/html; charset=utf-8", NULL, 0 };
    if (has_query_key(uri, "data")) {
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

static void *fcgi_loop(void *arg)
{
    int sock = (int)(long)arg;
    FCGX_Request req;
    if (FCGX_InitRequest(&req, sock, 0)) { syslog(LOG_ERR, "web: FCGX_InitRequest failed"); return NULL; }
    while (FCGX_Accept_r(&req) == 0) {
        web_reply r = web_route(FCGX_GetParam("REQUEST_URI", req.envp), g_html);
        FCGX_FPrintF(req.out, "Status: %d\r\nContent-Type: %s\r\nCache-Control: no-store\r\n"
                              "X-Content-Type-Options: nosniff\r\nContent-Length: %lu\r\n\r\n",  /* libfcgi printf has no %zu */
                     r.status, r.content_type, (unsigned long)r.len);
        if (r.body) FCGX_PutStr(r.body, (int)r.len, req.out);
        free(r.body);
        FCGX_Finish_r(&req);
    }
    return NULL;
}

int web_start(const char *html_path)
{
    const char *path = getenv("FCGI_SOCKET_NAME");
    if (!path) {
        syslog(LOG_WARNING, "web: FCGI_SOCKET_NAME not set - dashboard unavailable");
        return -1;
    }
    snprintf(g_html, sizeof g_html, "%s", html_path);
    if (FCGX_Init()) { syslog(LOG_ERR, "web: FCGX_Init failed"); return -1; }
    int sock = FCGX_OpenSocket(path, 5);
    if (sock < 0) { syslog(LOG_ERR, "web: cannot open %s", path); return -1; }
    pthread_t th;
    if (pthread_create(&th, NULL, fcgi_loop, (void *)(long)sock)) return -1;
    pthread_detach(th);
    syslog(LOG_INFO, "web: dashboard at /local/aurix/aurix.cgi");
    return 0;
}
#endif
