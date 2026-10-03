/* AURIX - build a face template from an enrolment photo, on the camera (same detector, alignment and
 * embedder as live recognition, so enrolment and matching always agree). */
#ifndef AURIX_ENROLL_H
#define AURIX_ENROLL_H
#include <stddef.h>
#include <stdint.h>
#include "detect.h"
#include "embed.h"

/* JPEG -> template. The caller holds the model lock. 0 = ok; else why says what to fix in the photo. */
int enroll_photo(aurix_detector *det, aurix_embedder *emb, const unsigned char *jpeg, size_t len,
                 int8_t *out, uint32_t dim, char *why, size_t why_len);

#endif
