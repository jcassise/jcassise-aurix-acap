/* AURIX - HTTPS transport for the AURIX-Pharos protocol (libcurl + OpenSSL).
 *
 * Server trust, from the commissioning "Server certificate" setting:
 *   empty                      normal CA validation (CA bundle on the device)
 *   sha256//<base64>           SPKI pin (public key) - survives certificate renewal with the same key
 *   64 hex chars (':' allowed) SHA-256 fingerprint of the server certificate. Checked once with a
 *                              bare TLS handshake (no HTTP sent), then converted to an SPKI pin.
 *   -----BEGIN CERTIFICATE---- the server certificate itself; its SPKI is pinned.
 * Two pins may be given separated by ';' (rotation). With a pin, hostname/CA checks are replaced
 * by the pin, which is enforced on every connection. A mismatch is a hard failure. */
#ifndef AURIX_PHAROS_HTTP_H
#define AURIX_PHAROS_HTTP_H
#include <stddef.h>

typedef enum {
    PH_OK = 0,
    PH_ERR_NETWORK,      /* DNS, connect, timeout */
    PH_ERR_TLS_PIN,      /* server key/certificate does not match the commissioned pin */
    PH_ERR_TLS_VERIFY,   /* CA validation failed (no pin configured) */
    PH_ERR_CONFIG,       /* bad URL / pin text */
} ph_err;

typedef struct {
    ph_err err;
    long status;               /* HTTP status, 0 if no response */
    char *body;                /* NUL-terminated, malloc'd (may contain binary for JPEG) */
    size_t body_len;
    int retry_after_s;         /* -1 if absent */
    int json;                  /* Content-Type is application/json */
    char errmsg[256];
} ph_response;

typedef struct ph_client ph_client;

/* base_url: e.g. https://pharos.site.local (the /aurix/v1 suffix is appended).
 * trust: commissioning certificate setting (see above). Returns NULL with *why filled on error. */
ph_client *ph_client_new(const char *base_url, const char *device_id, const char *token,
                         const char *trust, char *why, size_t why_len);
void ph_client_free(ph_client *c);

/* For a fingerprint trust setting: performs the pre-flight handshake if not done yet.
 * Returns PH_OK when every later request is protected by an SPKI pin (or CA validation). */
ph_err ph_client_prepare(ph_client *c, char *why, size_t why_len);

/* Re-base after /hello returned endpoints.base (absolute URL including /aurix/v1). */
void ph_client_set_endpoint(ph_client *c, const char *full_base);

/* method "GET"/"POST"/"PUT"; path relative to the endpoint ("/hello").
 * body/ctype may be NULL. Response must be freed with ph_response_free. */
ph_response ph_request(ph_client *c, const char *method, const char *path,
                       const void *body, size_t body_len, const char *ctype, long timeout_s);
void ph_response_free(ph_response *r);

/* Exposed for tests: SPKI pin ("sha256//...") of a PEM or DER certificate. */
int ph_spki_pin_from_cert(const unsigned char *data, size_t len, int is_pem, char *out, size_t out_len);

#endif
