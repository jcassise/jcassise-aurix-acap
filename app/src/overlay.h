/* AURIX - live-view overlay (axoverlay + cairo). Boxes are drawn on every video stream.
 *   green  = matched, allowed       red   = matched, threat      amber = matched, concern
 *   blue   = identified as unknown  grey  = face too small / not yet checked */
#ifndef AURIX_OVERLAY_H
#define AURIX_OVERLAY_H
#include "match.h"

typedef enum { OV_PENDING = 0, OV_UNKNOWN, OV_ALLOW, OV_THREAT, OV_CONCERN } overlay_state;

typedef struct {
    float x0, y0, x1, y1;          /* normalised 0..1 in the analysed frame */
    overlay_state state;
    char label[AURIX_ID_LEN + 16];
} overlay_box;

#define OVERLAY_MAX_BOXES 16

/* Call from the GLib main-loop thread. Returns 0 if overlays are available. */
int  overlay_init(void);
void overlay_cleanup(void);

/* Thread-safe: replaces the current boxes and schedules a redraw on the main loop. */
void overlay_publish(const overlay_box *boxes, int n);

#endif
