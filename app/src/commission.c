#include "commission.h"
#include "pharos_http.h"
#include <ctype.h>
#include <fcntl.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void path_of(const char *dir, char *out, size_t n) { snprintf(out, n, "%s/commission.json", dir); }

int commission_load(const char *dir, commission *c)
{
    char p[300];
    path_of(dir, p, sizeof p);
    memset(c, 0, sizeof *c);
    json_t *o = json_load_file(p, 0, NULL);
    if (!o) return -1;
    const char *u = json_string_value(json_object_get(o, "url")), *d = json_string_value(json_object_get(o, "deviceId")),
               *t = json_string_value(json_object_get(o, "token")), *s = json_string_value(json_object_get(o, "cert"));
    snprintf(c->url, sizeof c->url, "%s", u ? u : "");
    snprintf(c->device_id, sizeof c->device_id, "%s", d ? d : "");
    snprintf(c->token, sizeof c->token, "%s", t ? t : "");
    snprintf(c->trust, sizeof c->trust, "%s", s ? s : "");
    json_decref(o);
    return c->url[0] && c->device_id[0] && c->token[0] ? 0 : -1;
}

int commission_save(const char *dir, const commission *c)
{
    mkdir(dir, 0700);
    char p[300], tmp[310];
    path_of(dir, p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    json_t *o = json_pack("{s:s,s:s,s:s,s:s}", "url", c->url, "deviceId", c->device_id, "token", c->token, "cert", c->trust);
    char *txt = json_dumps(o, JSON_COMPACT);
    json_decref(o);
    if (!txt) return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int ok = fd >= 0 && write(fd, txt, strlen(txt)) == (ssize_t)strlen(txt) && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    explicit_bzero(txt, strlen(txt));
    free(txt);
    if (!ok || rename(tmp, p)) { unlink(tmp); return -1; }
    return 0;
}

int commission_clear(const char *dir)
{
    char p[300];
    path_of(dir, p, sizeof p);
    return unlink(p) == 0 ? 0 : -1;
}

int commission_validate(const commission *c, char *why, size_t n)
{
    if (strncasecmp(c->url, "https://", 8)) { snprintf(why, n, "Pharos address must start with https://"); return -1; }
    if (strlen(c->url) < 12) { snprintf(why, n, "Pharos address is incomplete - include the host name or IP address"); return -1; }
    for (const char *p = c->url; *p; p++)
        if (isspace((unsigned char)*p)) { snprintf(why, n, "Pharos address must not contain spaces"); return -1; }
    size_t dl = strlen(c->device_id);
    if (!dl || dl > 64 || !isalnum((unsigned char)c->device_id[0])) { snprintf(why, n, "Device ID: 1-64 characters, starting with a letter or digit"); return -1; }
    for (const char *p = c->device_id; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') { snprintf(why, n, "Device ID may only use letters, digits, '.', '_' and '-'"); return -1; }
    if (strlen(c->token) < 8) { snprintf(why, n, "Device token is missing or too short"); return -1; }
    for (const char *p = c->token; *p; p++)
        if ((unsigned char)*p < 0x21 || (unsigned char)*p > 0x7e) { snprintf(why, n, "Device token contains spaces or unusual characters - copy it again from Pharos"); return -1; }
    return ph_trust_validate(c->trust, why, n);
}

json_t *commission_cert_summary(const char *trust)
{
    json_t *o = json_object();
    while (trust && isspace((unsigned char)*trust)) trust++;
    if (!trust || !*trust) { json_object_set_new(o, "kind", json_string("none")); return o; }
    if (!strstr(trust, "-----BEGIN CERTIFICATE-----")) {
        json_object_set_new(o, "kind", json_string(strstr(trust, "sha256//") || (strlen(trust) == 44 && trust[43] == '=') ? "pin" : "fingerprint"));
        json_object_set_new(o, "value", json_stringn(trust, strlen(trust) > 200 ? 200 : strlen(trust)));
        return o;
    }
    json_object_set_new(o, "kind", json_string("pem"));
    BIO *b = BIO_new_mem_buf(trust, -1);
    X509 *x = b ? PEM_read_bio_X509(b, NULL, NULL, NULL) : NULL;
    BIO_free(b);
    if (!x) return o;
    char cn[256] = "";
    X509_NAME_get_text_by_NID(X509_get_subject_name(x), NID_commonName, cn, sizeof cn);
    json_object_set_new(o, "subject", json_string(cn));
    unsigned char *der = NULL, h[SHA256_DIGEST_LENGTH];
    int dl = i2d_X509(x, &der);
    if (dl > 0) {
        SHA256(der, (size_t)dl, h);
        char fp[3 * 32];
        for (int i = 0; i < 32; i++) snprintf(fp + 3 * i, 4, "%02X%s", h[i], i < 31 ? ":" : "");
        json_object_set_new(o, "fingerprint", json_string(fp));
        char pin[80];
        if (!ph_spki_pin_from_cert(der, (size_t)dl, 0, pin, sizeof pin)) json_object_set_new(o, "pin", json_string(pin));
        OPENSSL_free(der);
    }
    struct tm tm;
    if (ASN1_TIME_to_tm(X509_get0_notAfter(x), &tm) == 1) {
        long long t = (long long)timegm(&tm) * 1000;
        json_object_set_new(o, "notAfter", json_integer(t));
        json_object_set_new(o, "expired", json_boolean(t < (long long)time(NULL) * 1000));
    }
    X509_free(x);
    return o;
}
