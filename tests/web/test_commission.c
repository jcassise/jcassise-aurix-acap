/* Commissioning API: real web.c routing + commission store + certificate parsing. */
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "commission.h"
#include "web.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } else printf("PASS %s\n", #c); } while (0)
static int changed;
static void on_changed(void *u) { (void)u; changed++; }
static void on_status(char *o, size_t n, void *u) { (void)u; snprintf(o, n, "Connected"); }

static json_t *call(const web_commissioning *wc, const char *method, const char *body, int csrf, int *status)
{
    web_request rq = { method, "/local/aurix/aurix.cgi?commission", body, body ? strlen(body) : 0, csrf };
    web_reply r = web_route(&rq, "/nonexistent", wc);
    *status = r.status;
    json_t *j = json_loadb(r.body, r.len, 0, NULL);
    if (r.body && strstr(r.body, "secret-token-123")) { fails++; printf("FAIL token echoed in response\n"); }
    free(r.body);
    return j;
}

int main(int argc, char **argv)
{
    (void)argc;
    char dir[] = "/tmp/aurix-commission-XXXXXX";
    if (!mkdtemp(dir)) return 2;
    FILE *f = fopen(argv[1], "r");                 /* test certificate PEM */
    char pem[8192] = "";
    size_t n = fread(pem, 1, sizeof pem - 1, f); pem[n] = 0; fclose(f);
    web_commissioning wc = { dir, on_status, on_changed, NULL };
    int st;
    json_t *j;

    j = call(&wc, "GET", NULL, 0, &st);
    CHECK(st == 200 && json_is_false(json_object_get(j, "commissioned")), "fresh camera is not commissioned");
    json_decref(j);

    char body[16384];
    json_t *b = json_pack("{s:s,s:s,s:s,s:s}", "url", "https://pharos.site.local", "deviceId", "aurix-lobby-01",
                          "token", "secret-token-123", "cert", pem);
    char *bs = json_dumps(b, 0); snprintf(body, sizeof body, "%s", bs); free(bs); json_decref(b);

    j = call(&wc, "POST", body, 0, &st);
    CHECK(st == 403 && !strcmp(json_string_value(json_object_get(json_object_get(j, "error"), "code")), "missing_request_header"),
          "POST without X-AURIX-Request is refused with a code (got %d)", st);
    json_decref(j);
    CHECK(changed == 0, "nothing changed after refused POST");

    j = call(&wc, "POST", "{\"url\":\"https://pharos.site.local\",\"deviceId\":\"aurix-lobby-01\",\"token\":\"secret-token-123\",\"cert\":\"pharos.site.local\"}", 1, &st);
    CHECK(st == 422 && strstr(json_string_value(json_object_get(json_object_get(j, "error"), "message")), "got \"pharos.site.local\"") &&
          !strcmp(json_string_value(json_object_get(json_object_get(j, "error"), "code")), "invalid_cert"),
          "bad certificate is rejected with a code and what was received (got %d)", st);
    json_decref(j);

    j = call(&wc, "POST", "{\"url\":\"https://p\",\"deviceId\":\"aurix-lobby-01\",\"token\":\"secret-token-123\"}", 1, &st);
    CHECK(st == 422 && strstr(json_string_value(json_object_get(json_object_get(j, "error"), "message")), "incomplete") &&
          !strcmp(json_string_value(json_object_get(json_object_get(j, "error"), "code")), "invalid_url"),
          "incomplete address gets an accurate message and code");
    json_decref(j);
    j = call(&wc, "POST", "{\"url\":\"https://pharos.site.local\",\"deviceId\":\"aurix-lobby-01\",\"token\":\"secret-token-123\",\"cert\":\"\"}", 1, &st);
    CHECK(st == 200 && !strcmp(json_string_value(json_object_get(json_object_get(j, "cert"), "kind")), "none"),
          "an empty server key means public-CA validation (shown as kind none), not blind trust");
    json_decref(j);
    j = call(&wc, "POST", "{\"clear\":true}", 1, &st);
    changed = 0;                                   /* the counts below start from a clean camera */
    json_decref(j);

    j = call(&wc, "POST", body, 1, &st);
    json_t *cert = json_object_get(j, "cert");
    CHECK(st == 200 && json_is_true(json_object_get(j, "commissioned")) && json_is_true(json_object_get(j, "tokenSet")),
          "valid commissioning saved (got %d)", st);
    CHECK(!strcmp(json_string_value(json_object_get(cert, "kind")), "pem") &&
          !strcmp(json_string_value(json_object_get(cert, "subject")), "pharos.site.local") &&
          strlen(json_string_value(json_object_get(cert, "fingerprint"))) == 95, "certificate summary: subject and fingerprint");
    CHECK(!strcmp(json_string_value(json_object_get(j, "status")), "Connected"), "live status included");
    CHECK(changed == 1, "change hook called once");
    json_decref(j);

    char path[300]; struct stat sb;
    snprintf(path, sizeof path, "%s/commission.json", dir);
    CHECK(stat(path, &sb) == 0 && (sb.st_mode & 0777) == 0600, "stored with owner-only permissions");

    j = call(&wc, "POST", "{\"url\":\"https://pharos2.site.local\",\"deviceId\":\"aurix-lobby-01\",\"token\":\"\"}", 1, &st);
    json_decref(j);
    commission c;
    CHECK(st == 200 && commission_load(dir, &c) == 0 && !strcmp(c.token, "secret-token-123") && strstr(c.trust, "BEGIN CERTIFICATE")
          && !strcmp(c.url, "https://pharos2.site.local"), "blank token and absent cert keep the saved ones");

    {   /* commissioning with a PUBLIC KEY: accepted, summarised as a key with its pin */
        FILE *kf = popen("openssl x509 -in /tmp/aurix-tc.pem -pubkey -noout 2>/dev/null || openssl x509 -in /tmp/tc.pem -pubkey -noout", "r");
        char key[2048] = ""; size_t kn = fread(key, 1, sizeof key - 1, kf); key[kn] = 0; pclose(kf);
        json_t *kb = json_pack("{s:s,s:s,s:s}", "url", "https://pharos.site.local", "deviceId", "aurix-lobby-01", "cert", key);
        char *ks = json_dumps(kb, 0);
        j = call(&wc, "POST", ks, 1, &st);
        free(ks); json_decref(kb);
        json_t *kc = json_object_get(j, "cert");
        CHECK(st == 200 && !strcmp(json_string_value(json_object_get(kc, "kind")), "key") &&
              !strncmp(json_string_value(json_object_get(kc, "pin")), "sha256//", 8), "public key accepted and summarised with its pin (got %d)", st);
        json_decref(j);
    }

    j = call(&wc, "POST", "{\"clear\":true}", 1, &st);
    CHECK(st == 200 && json_is_false(json_object_get(j, "commissioned")) && stat(path, &sb) != 0, "disconnect removes the saved connection");
    json_decref(j);

    web_request rq = { "POST", "/local/aurix/aurix.cgi?data", "{}", 2, 1 };
    web_reply r = web_route(&rq, "/nonexistent", &wc);
    CHECK(r.status == 405, "POST elsewhere is refused");
    free(r.body);
    printf(fails ? "%d failure(s)\n" : "all commissioning checks passed\n", fails);
    return fails != 0;
}
