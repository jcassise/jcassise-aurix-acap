/* AURIX - the pictures sent with an event: a face close-up and the scene (pure C, host-testable). */
#ifndef AURIX_SNAPSHOT_H
#define AURIX_SNAPSHOT_H
#include <stddef.h>
#include "image.h"
#include "tracker.h"

/* Face close-up: centred on the face (landmarks when known), ~1.5x the face, at least 240 px or native
 * (never upscaled), at most 480 px tall. JPEG, malloc'd; NULL if the face is too small or at the edge. */
unsigned char *snapshot_face(const aurix_image *frame, const trk_track *t, size_t *len);

/* Scene: 1280 wide in the camera's own shape (1280x960 on a 4:3 sensor, 1280x720 on 16:9). JPEG, malloc'd. */
unsigned char *snapshot_scene(const aurix_image *frame, size_t *len);

#endif
