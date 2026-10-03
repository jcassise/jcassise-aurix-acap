/* AURIX - JPEG decoding for enrolment photos (stb_image, JPEG only). */
#ifndef AURIX_JPEG_H
#define AURIX_JPEG_H
#include <stddef.h>
#include "image.h"

/* Decodes a JPEG to packed RGB. Images larger than max_side are scaled down (keeps memory and
 * detection time bounded; faces in enrolment photos are large). Returns 0 and a malloc'd
 * out->data, or -1 with why filled. */
int jpeg_decode_rgb(const unsigned char *data, size_t len, int max_side, aurix_image *out, char *why, size_t why_len);

/* Encodes packed RGB to JPEG (quality 1-100). Returns a malloc'd buffer and its size, or NULL. */
unsigned char *jpeg_encode_rgb(const aurix_image *img, int quality, size_t *out_len);

#endif
