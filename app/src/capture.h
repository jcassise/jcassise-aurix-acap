/* AURIX - VDO frame capture, always delivered to callers as packed RGB888. */
#ifndef AURIX_CAPTURE_H
#define AURIX_CAPTURE_H
#include "image.h"

typedef struct aurix_capture aurix_capture;

aurix_capture *capture_open(unsigned width, unsigned height, double fps);
/* Blocks for the next frame. out->data stays valid until the next call. */
int capture_next(aurix_capture *c, aurix_image *out);
void capture_close(aurix_capture *c);

/* Timing of the last capture_next(): waiting for the camera vs converting the frame (ms). */
void capture_last_timing(const aurix_capture *c, double *wait_ms, double *convert_ms);

#endif
