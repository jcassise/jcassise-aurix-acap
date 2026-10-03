/* AURIX - on-camera dashboard over the camera's own web server (FastCGI, httpConfig "aurix.cgi").
 *   /local/aurix/aurix.cgi        dashboard page (web/dashboard.html from the package)
 *   /local/aurix/aurix.cgi?data   live metrics JSON (polled by the page)
 * Access is the camera's own login (admin), configured in the manifest. */
#ifndef AURIX_WEB_H
#define AURIX_WEB_H
#include <stddef.h>

typedef struct {
    int status;                 /* HTTP status */
    const char *content_type;
    char *body;                 /* malloc'd */
    size_t len;
} web_reply;

/* Pure routing (host-testable). html_path: dashboard file. */
web_reply web_route(const char *request_uri, const char *html_path);

/* Starts the FastCGI loop in a thread if FCGI_SOCKET_NAME is set. Returns 0 on success. */
int web_start(const char *html_path);

#endif
