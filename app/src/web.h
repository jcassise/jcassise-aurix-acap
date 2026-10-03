/* AURIX - on-camera dashboard over the camera's own web server (FastCGI, httpConfig "aurix.cgi").
 *   /local/aurix/aurix.cgi        dashboard page (web/dashboard.html from the package)
 *   /local/aurix/aurix.cgi?data   live metrics JSON (polled by the page)
 * Access is the camera's own login (admin), configured in the manifest. */
#ifndef AURIX_WEB_H
#define AURIX_WEB_H
#include <jansson.h>
#include <stddef.h>

typedef struct {
    int status;                 /* HTTP status */
    const char *content_type;
    char *body;                 /* malloc'd */
    size_t len;
} web_reply;

typedef struct {
    const char *method;         /* "GET" / "POST" */
    const char *uri;            /* REQUEST_URI */
    const char *body;           /* POST body (may be NULL) */
    size_t body_len;
    int csrf_header;            /* request carried "X-AURIX-Request: 1" */
} web_request;

/* Commissioning: where commission.json lives, how to read live status, and who to tell on change. */
typedef struct {
    const char *state_dir;
    void (*status)(char *out, size_t n, void *user);     /* current Pharos state text */
    void (*changed)(void *user);                          /* called after a save or clear (any thread) */
    void *user;
    /* settings console: describe (caller owns the result) and save {key: value|null} (0 = ok, else why) */
    json_t *(*settings_get)(void *user);
    int (*settings_set)(const json_t *values, char *why, size_t why_len, void *user);
} web_commissioning;

/* Pure routing (host-testable). html_path: dashboard file.
 *   GET  ?data         live metrics
 *   GET  ?commission   Pharos connection (never returns the token)
 *   POST ?commission   save {url, deviceId, token?, cert?} or {clear:true}; needs X-AURIX-Request: 1
 *   GET  ?settings     every setting with its value and where it comes from (pharos | local | default)
 *   POST ?settings     {key: value|null}; keys Pharos manages are refused; needs X-AURIX-Request: 1 */
web_reply web_route(const web_request *rq, const char *html_path, const web_commissioning *wc);

/* Starts the FastCGI loop in a thread if FCGI_SOCKET_NAME is set. Returns 0 on success. */
int web_start(const char *html_path, const web_commissioning *wc);

#endif
