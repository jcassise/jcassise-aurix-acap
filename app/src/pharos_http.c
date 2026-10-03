#include "pharos_http.h"
#include <ctype.h>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct ph_client {
    char base[512];           /* scheme://host[:port]/aurix/v1 */
    char auth[1100];          /* "Authorization: Bearer ..." */
    char devhdr[128];         /* "X-AURIX-Device: ..." */
    char pins[200];           /* "sha256//a;sha256//b" (curl format), empty = CA validation */
    char fingerprint[2][65];  /* lowercase hex, pending pre-flight */
    int n_fp;
    int prepared;
    CURL *curl;               /* reused: keeps the TLS connection alive between status posts */
    char tls[128];            /* "TLSv1.3 / TLS_AES_256_GCM_SHA384" from the last handshake */
};

static pthread_once_t once = PTHREAD_ONCE_INIT;
static void global_init(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

static void set_err(char *why, size_t n, const char *m)
{
    if (why && n) snprintf(why, n, "%s", m);
}

/* ---------- pins ---------- */

int ph_spki_pin_from_cert(const unsigned char *data, size_t len, int is_pem, char *out, size_t out_len)
{
    X509 *x = NULL;
    if (is_pem) {
        BIO *b = BIO_new_mem_buf(data, (int)len);
        if (b) x = PEM_read_bio_X509(b, NULL, NULL, NULL);
        BIO_free(b);
    } else {
        const unsigned char *p = data;
        x = d2i_X509(NULL, &p, (long)len);
    }
    if (!x) return -1;
    unsigned char *der = NULL;
    int dl = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(x), &der);
    X509_free(x);
    if (dl <= 0) return -1;
    unsigned char h[SHA256_DIGEST_LENGTH];
    SHA256(der, (size_t)dl, h);
    OPENSSL_free(der);
    unsigned char b64[64];
    int bl = EVP_EncodeBlock(b64, h, sizeof h);
    if (bl <= 0 || (size_t)bl + 9 > out_len) return -1;
    snprintf(out, out_len, "sha256//%s", b64);
    return 0;
}

static int cert_fingerprint_hex(const unsigned char *der, size_t len, char out[65])
{
    unsigned char h[SHA256_DIGEST_LENGTH];
    SHA256(der, len, h);
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", h[i]);
    return 0;
}

static int parse_hex_fp(const char *s, size_t n, char out[65])
{
    int k = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == ':' || c == ' ') continue;
        if (!isxdigit((unsigned char)c) || k >= 64) return -1;
        out[k++] = (char)tolower((unsigned char)c);
    }
    out[k] = 0;
    return k == 64 ? 0 : -1;
}


/* "sha256 Fingerprint=AB:CD..." (openssl), "SHA-256: ...", "Fingerprint: ..." -> value only. */
static char *strip_label(char *t)
{
    static const char *labels[] = { "sha256 fingerprint", "sha-256 fingerprint", "sha256", "sha-256",
                                    "fingerprint", "spki", "pin", NULL };
    for (int again = 1; again;) {
        again = 0;
        while (*t == ' ' || *t == '\t') t++;
        for (int i = 0; labels[i]; i++) {
            size_t n = strlen(labels[i]);
            if (!strncasecmp(t, labels[i], n) && (t[n] == '=' || t[n] == ':' || t[n] == ' ')) {
                /* never strip the "sha256" of a "sha256//" pin */
                if (t[n] == ':' && t[n + 1] == '/') continue;
                t += n + 1;
                again = 1;
                break;
            }
        }
    }
    return t;
}

static int is_b64_pin(const char *t, size_t n)
{
    if (n != 44 || t[43] != '=') return 0;
    for (size_t i = 0; i < 43; i++)
        if (!isalnum((unsigned char)t[i]) && t[i] != '+' && t[i] != '/') return 0;
    return 1;
}


/* Any PEM block (CERTIFICATE, PUBLIC KEY, RSA PUBLIC KEY) or a DER blob -> SPKI pin.
 * Whitespace inside the base64 is ignored, so one-line pastes work. Returns 0 on success. */
static int der_to_pin(const unsigned char *der, size_t len, char *out, size_t out_len)
{
    if (!ph_spki_pin_from_cert(der, len, 0, out, out_len)) return 0;          /* X.509 certificate */
    const unsigned char *p = der;
    EVP_PKEY *k = d2i_PUBKEY(NULL, &p, (long)len);                           /* SubjectPublicKeyInfo */
    if (!k) { p = der; k = d2i_PublicKey(EVP_PKEY_RSA, NULL, &p, (long)len); } /* PKCS#1 RSA key */
    if (!k) return -1;
    unsigned char *spki = NULL;
    int sl = i2d_PUBKEY(k, &spki);
    EVP_PKEY_free(k);
    if (sl <= 0) return -1;
    unsigned char h[SHA256_DIGEST_LENGTH], b64[64];
    SHA256(spki, (size_t)sl, h);
    OPENSSL_free(spki);
    if (EVP_EncodeBlock(b64, h, sizeof h) <= 0) return -1;
    snprintf(out, out_len, "sha256//%s", b64);
    return 0;
}

int ph_pin_from_pem_text(const char *text, char *out, size_t out_len)
{
    const char *b = strstr(text, "-----BEGIN ");
    if (!b) return -1;
    const char *body = strstr(b + 11, "-----");
    if (!body) return -1;
    body += 5;
    const char *e = strstr(body, "-----END");
    if (!e) return -1;
    size_t n = (size_t)(e - body);
    unsigned char *clean = malloc(n + 4), *der = malloc(n + 4);
    if (!clean || !der) { free(clean); free(der); return -1; }
    size_t k = 0;
    for (const char *q = body; q < e; q++)
        if (isalnum((unsigned char)*q) || *q == '+' || *q == '/' || *q == '=') clean[k++] = (unsigned char)*q;
    int pad = (k > 0 && clean[k - 1] == '=') + (k > 1 && clean[k - 2] == '=');
    int rc = -1;
    if (k && k % 4 == 0) {
        int dl = EVP_DecodeBlock(der, clean, (int)k);
        if (dl > pad) rc = der_to_pin(der, (size_t)(dl - pad), out, out_len);
    }
    free(clean);
    free(der);
    return rc;
}

static int add_pin(ph_client *c, const char *pin)
{
    size_t need = strlen(c->pins) + strlen(pin) + 2;
    if (need > sizeof c->pins) return -1;
    if (c->pins[0]) strcat(c->pins, ";");
    strcat(c->pins, pin);
    return 0;
}

static int parse_trust(ph_client *c, const char *trust, char *why, size_t wl)
{
    c->pins[0] = 0;
    c->n_fp = 0;
    if (!trust) return 0;
    while (*trust == ' ' || *trust == '\n' || *trust == '\r' || *trust == '\t') trust++;
    if (!*trust) return 0;
    if (strstr(trust, "-----BEGIN ")) {
        /* certificate or public key, multi-line or flattened into one line */
        char pin[80];
        if (ph_pin_from_pem_text(trust, pin, sizeof pin)) {
            set_err(why, wl, "Server certificate: the PEM block could not be read as a certificate or public key");
            return -1;
        }
        return add_pin(c, pin);
    }
    char buf[400];
    snprintf(buf, sizeof buf, "%s", trust);
    for (char *tok = strtok(buf, ";,\n"); tok; tok = strtok(NULL, ";,\n")) {
        tok = strip_label(tok);
        size_t n = strlen(tok);
        while (n && (tok[n - 1] == ' ' || tok[n - 1] == '\r' || tok[n - 1] == '\t')) tok[--n] = 0;
        if (!n) continue;
        if (!strncmp(tok, "sha256//", 8)) {
            for (char *q = tok + 8; *q; q++) if (*q == ' ') *q = '+';     /* web forms turn '+' into ' ' */
            if (!is_b64_pin(tok + 8, n - 8) || add_pin(c, tok)) {
                set_err(why, wl, "Server certificate: sha256// pin must be 44 base64 characters");
                return -1;
            }
        } else if (is_b64_pin(tok, n)) {                                   /* bare SPKI pin */
            char pin[64];
            snprintf(pin, sizeof pin, "sha256//%s", tok);
            if (add_pin(c, pin)) { set_err(why, wl, "Server certificate: too many pins"); return -1; }
        } else if (c->n_fp < 2 && !parse_hex_fp(tok, n, c->fingerprint[c->n_fp])) {
            c->n_fp++;
        } else {
            char m[200];
            snprintf(m, sizeof m, "Server certificate: not a PEM, SHA-256 fingerprint or sha256// pin (got \"%.24s%s\", %zu characters)",
                     tok, n > 24 ? "..." : "", n);
            set_err(why, wl, m);
            return -1;
        }
    }
    return 0;
}

int ph_trust_validate(const char *trust, char *why, size_t wl)
{
    ph_client tmp;
    memset(&tmp, 0, sizeof tmp);
    return parse_trust(&tmp, trust, why, wl);
}

/* ---------- client ---------- */

ph_client *ph_client_new(const char *base_url, const char *device_id, const char *token,
                         const char *trust, char *why, size_t wl)
{
    pthread_once(&once, global_init);
    if (!base_url || strncasecmp(base_url, "https://", 8)) {
        set_err(why, wl, "Pharos URL must start with https://");
        return NULL;
    }
    if (!device_id || !*device_id || !token || !*token) {
        set_err(why, wl, "Device ID and device token are required");
        return NULL;
    }
    ph_client *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    size_t bl = strlen(base_url);
    while (bl && base_url[bl - 1] == '/') bl--;
    if (bl > 9 && !strncmp(base_url + bl - 9, "/aurix/v1", 9)) bl -= 9;   /* tolerate a pasted full base */
    snprintf(c->base, sizeof c->base, "%.*s/aurix/v1", (int)bl, base_url);
    snprintf(c->auth, sizeof c->auth, "Authorization: Bearer %s", token);
    snprintf(c->devhdr, sizeof c->devhdr, "X-AURIX-Device: %s", device_id);
    if (parse_trust(c, trust, why, wl)) { free(c); return NULL; }
    c->prepared = c->n_fp == 0;
    c->curl = curl_easy_init();
    if (!c->curl) { free(c); set_err(why, wl, "curl init failed"); return NULL; }
    return c;
}

void ph_client_free(ph_client *c)
{
    if (!c) return;
    if (c->curl) curl_easy_cleanup(c->curl);
    explicit_bzero(c->auth, sizeof c->auth);
    free(c);
}

void ph_client_set_endpoint(ph_client *c, const char *full_base)
{
    if (!full_base || strncasecmp(full_base, "https://", 8)) return;   /* never downgrade */
    size_t n = strlen(full_base);
    while (n && full_base[n - 1] == '/') n--;
    snprintf(c->base, sizeof c->base, "%.*s", (int)n, full_base);
}

static void apply_tls(ph_client *c, CURL *h)
{
    if (c->pins[0]) {
        /* The pin replaces CA/hostname validation (self-signed or IP-address servers);
         * libcurl enforces CURLOPT_PINNEDPUBLICKEY on every handshake regardless. */
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(h, CURLOPT_PINNEDPUBLICKEY, c->pins);
    } else {
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
    }
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(h, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
}

ph_err ph_client_prepare(ph_client *c, char *why, size_t wl)
{
    if (c->prepared) return PH_OK;
    /* Pre-flight: TLS handshake only (CONNECT_ONLY), no HTTP and no token sent. */
    CURL *h = curl_easy_init();
    if (!h) return PH_ERR_NETWORK;
    curl_easy_setopt(h, CURLOPT_URL, c->base);
    curl_easy_setopt(h, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(h, CURLOPT_CERTINFO, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    char ebuf[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, ebuf);
    CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK) {
        if (why && wl) snprintf(why, wl, "%s%s%s", curl_easy_strerror(rc), ebuf[0] ? ": " : "", ebuf);
        curl_easy_cleanup(h);
        return PH_ERR_NETWORK;
    }
    struct curl_certinfo *ci = NULL;
    ph_err res = PH_ERR_TLS_PIN;
    set_err(why, wl, "Server certificate does not match the commissioned fingerprint");
    if (curl_easy_getinfo(h, CURLINFO_CERTINFO, &ci) == CURLE_OK && ci && ci->num_of_certs > 0) {
        for (struct curl_slist *s = ci->certinfo[0]; s; s = s->next) {
            if (strncmp(s->data, "Cert:", 5)) continue;
            const char *pem = s->data + 5;
            BIO *b = BIO_new_mem_buf(pem, -1);
            X509 *x = b ? PEM_read_bio_X509(b, NULL, NULL, NULL) : NULL;
            BIO_free(b);
            if (!x) break;
            unsigned char *der = NULL;
            int dl = i2d_X509(x, &der);
            char fp[65] = "";
            if (dl > 0) cert_fingerprint_hex(der, (size_t)dl, fp);
            int match = 0;
            for (int i = 0; i < c->n_fp; i++) match |= !strcmp(fp, c->fingerprint[i]);
            char pin[80];
            if (match && dl > 0 && !ph_spki_pin_from_cert(der, (size_t)dl, 0, pin, sizeof pin) && !add_pin(c, pin)) {
                c->prepared = 1;
                res = PH_OK;
                set_err(why, wl, "");
            }
            OPENSSL_free(der);
            X509_free(x);
            break;
        }
    }
    curl_easy_cleanup(h);
    return res;
}

static int on_debug(CURL *h, curl_infotype type, char *data, size_t size, void *user)
{
    (void)h;
    ph_client *c = user;
    static const char tag[] = "SSL connection using ";
    if (type == CURLINFO_TEXT && size > sizeof tag && !strncmp(data, tag, sizeof tag - 1)) {
        size_t n = size - (sizeof tag - 1);
        while (n && (data[sizeof tag - 1 + n - 1] == '\n' || data[sizeof tag - 1 + n - 1] == '\r')) n--;
        if (n >= sizeof c->tls) n = sizeof c->tls - 1;
        memcpy(c->tls, data + sizeof tag - 1, n);
        c->tls[n] = 0;
    }
    return 0;
}

typedef struct { char *p; size_t n, cap; } buf_t;

static size_t on_body(char *d, size_t sz, size_t nm, void *u)
{
    buf_t *b = u;
    size_t n = sz * nm;
    if (b->n + n + 1 > b->cap) {
        size_t cap = (b->n + n + 1) * 2;
        if (cap > 16u << 20) return 0;     /* refuse absurd bodies (> 16 MiB) */
        char *q = realloc(b->p, cap);
        if (!q) return 0;
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
    b->p[b->n] = 0;
    return n;
}

typedef struct { int retry_after; int json; } hdr_t;

static size_t on_header(char *d, size_t sz, size_t nm, void *u)
{
    hdr_t *h = u;
    size_t n = sz * nm;
    if (n > 12 && !strncasecmp(d, "Retry-After:", 12)) h->retry_after = atoi(d + 12);
    if (n > 13 && !strncasecmp(d, "Content-Type:", 13)) {
        const char *v = d + 13;
        while (*v == ' ') v++;
        h->json = !strncasecmp(v, "application/json", 16);
    }
    return n;
}

ph_response ph_request(ph_client *c, const char *method, const char *path,
                       const void *body, size_t body_len, const char *ctype, long timeout_s)
{
    ph_response r = { .err = PH_OK, .retry_after_s = -1 };
    if (!c->prepared) {
        r.err = PH_ERR_TLS_PIN;
        snprintf(r.errmsg, sizeof r.errmsg, "server identity not verified yet");
        return r;
    }
    CURL *h = c->curl;
    curl_easy_reset(h);
    char url[1024];
    snprintf(url, sizeof url, "%s%s", c->base, path);
    struct curl_slist *hl = NULL;
    hl = curl_slist_append(hl, c->auth);
    hl = curl_slist_append(hl, c->devhdr);
    hl = curl_slist_append(hl, "Accept: application/json");
    hl = curl_slist_append(hl, "Expect:");
    char ct[96];
    if (body) {
        snprintf(ct, sizeof ct, "Content-Type: %s", ctype ? ctype : "application/json");
        hl = curl_slist_append(hl, ct);
    }
    buf_t b = { 0 };
    hdr_t hd = { -1, 0 };
    char ebuf[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, ebuf);
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method);
    if (body) {
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_len);
    } else if (!strcmp(method, "GET")) {
        curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
    }
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &hd);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, timeout_s > 0 ? timeout_s : 15L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);   /* never follow redirects with the token */
    curl_easy_setopt(h, CURLOPT_DEBUGFUNCTION, on_debug);  /* only reads the handshake summary line */
    curl_easy_setopt(h, CURLOPT_DEBUGDATA, c);
    curl_easy_setopt(h, CURLOPT_VERBOSE, 1L);
    apply_tls(c, h);

    CURLcode rc = curl_easy_perform(h);
    curl_slist_free_all(hl);
    if (rc == CURLE_SSL_PINNEDPUBKEYNOTMATCH) {
        r.err = PH_ERR_TLS_PIN;
        snprintf(r.errmsg, sizeof r.errmsg, "Server key does not match the commissioned pin");
    } else if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE
#ifdef CURLE_SSL_CACERT
               || rc == CURLE_SSL_CACERT
#endif
               ) {
        r.err = PH_ERR_TLS_VERIFY;
        snprintf(r.errmsg, sizeof r.errmsg, "Server certificate not trusted: %s (commission a pin)",
                 curl_easy_strerror(rc));
    } else if (rc != CURLE_OK) {
        r.err = PH_ERR_NETWORK;
        /* e.g. "SSL connect error: ... tlsv1 alert protocol version" - the detail is what diagnoses it */
        snprintf(r.errmsg, sizeof r.errmsg, "%.48s%s%.190s", curl_easy_strerror(rc), ebuf[0] ? ": " : "", ebuf);
    } else {
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &r.status);
    }
    r.body = b.p;
    r.body_len = b.n;
    r.retry_after_s = hd.retry_after;
    r.json = hd.json;
    return r;
}

int ph_client_tls_info(ph_client *c, char *out, size_t n)
{
    snprintf(out, n, "%s", c->tls);
    return c->tls[0] ? 0 : -1;
}

void ph_response_free(ph_response *r)
{
    free(r->body);
    r->body = NULL;
}
