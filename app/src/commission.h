/* AURIX - Pharos commissioning store (localdata/pharos/commission.json, mode 0600).
 * Written by the AURIX page; takes precedence over the Pharos* app settings when present. */
#ifndef AURIX_COMMISSION_H
#define AURIX_COMMISSION_H
#include <jansson.h>

typedef struct {
    char url[256], device_id[80], token[1024], trust[8192];
} commission;

int  commission_load(const char *dir, commission *c);          /* 0 if a saved commissioning exists */
int  commission_save(const char *dir, const commission *c);    /* atomic, 0600 */
int  commission_clear(const char *dir);

/* Validates fields; returns 0, or fills why and sets *code to a stable identifier for software:
 * invalid_url | invalid_device_id | invalid_token | invalid_cert. */
int  commission_validate(const commission *c, char *why, size_t len, const char **code);

/* Public description of a certificate setting (never secrets):
 * {kind: none|pem|fingerprint|pin, subject, fingerprint, pin, notAfter, expired} */
json_t *commission_cert_summary(const char *trust);

#endif
