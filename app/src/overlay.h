/* AURIX - live-view overlay (axoverlay + cairo). Boxes are drawn on every video stream.
 *   green  = matched, allowed       red   = matched, threat      amber = matched, concern
 *   blue   = identified as unknown  grey  = face too small / not yet checked
 *   magenta = known but not authorised here (virtual access)   orange-red = stranger in a restricted area */
#ifndef AURIX_OVERLAY_H
#define AURIX_OVERLAY_H
#include "match.h"

typedef enum { OV_PENDING = 0, OV_UNKNOWN, OV_ALLOW, OV_THREAT, OV_CONCERN, OV_DENIED, OV_ALERT } overlay_state;

typedef struct {
    float x0, y0, x1, y1;          /* normalised 0..1 in the analysed frame */
    overlay_state state;
    float confidence;              /* 0..1: how settled the identity is (drawn as strength) */
    float cx, cy, rx, ry;          /* face ellipse from the landmarks (normalised); ry <= 0 = use the box */
    char label[AURIX_ID_LEN + 24];
} overlay_box;

#define OVERLAY_MAX_BOXES 16

/* Call from the GLib main-loop thread. Returns 0 if overlays are available. */
int  overlay_init(void);
void overlay_cleanup(void);

/* Thread-safe: replaces the current boxes and schedules a redraw on the main loop. */
void overlay_publish(const overlay_box *boxes, int n);

/* Average drawing time per render since the last call, and how many renders (main loop only). */
void overlay_stats(double *avg_render_ms, unsigned *renders);

/* Full sensor (capture) size, as the overlay system reports it. 0 if the overlay is unavailable. */
void overlay_capture_size(int *w, int *h);

/* Fine-tuning per camera: y' = 0.5 + (y - 0.5) * scale_y + offset_y (normalised); scores on labels. */
void overlay_set_tuning(float offset_y, float scale_y, int show_scores);

#endif
