#!/bin/sh
# Renders the real overlay code at 3840x2160 with a stand-in axoverlay: no stray lines, no trails,
# jitter ignored. Needs: libcairo2-dev libglib2.0-dev
set -e
cd "$(dirname "$0")"
mkdir -p stubs
cat > stubs/axoverlay.h <<'H'
#include <glib.h>
enum axoverlay_position_type { AXOVERLAY_CUSTOM_NORMALIZED };
enum axoverlay_anchor_point { AXOVERLAY_ANCHOR_CENTER };
enum axoverlay_backend_type { AXOVERLAY_CAIRO_IMAGE_BACKEND };
enum axoverlay_colorspace { AXOVERLAY_COLORSPACE_ARGB32 };
struct axoverlay_stream_data { gint id, camera, width, height, rotation; };
typedef void (*axoverlay_render_callback)(gpointer, gint, struct axoverlay_stream_data*, enum axoverlay_position_type, gfloat, gfloat, gint, gint, gpointer);
typedef void (*axoverlay_adjustment_callback)(gint, struct axoverlay_stream_data*, enum axoverlay_position_type*, gfloat*, gfloat*, gint*, gint*, gpointer);
struct axoverlay_settings { axoverlay_render_callback render_callback; axoverlay_adjustment_callback adjustment_callback; void* select_callback; enum axoverlay_backend_type backend; };
struct axoverlay_overlay_data { enum axoverlay_position_type postype; enum axoverlay_anchor_point anchor_point; gfloat x, y; gboolean scale_to_stream; gint width, height; enum axoverlay_colorspace colorspace; };
gboolean axoverlay_is_backend_supported(enum axoverlay_backend_type);
void axoverlay_init_axoverlay_settings(struct axoverlay_settings*);
void axoverlay_init(struct axoverlay_settings*, GError**);
gint axoverlay_get_max_resolution_width(gint, GError**);
gint axoverlay_get_max_resolution_height(gint, GError**);
void axoverlay_init_overlay_data(struct axoverlay_overlay_data*);
gint axoverlay_create_overlay(struct axoverlay_overlay_data*, gpointer, GError**);
void axoverlay_destroy_overlay(gint, GError**);
void axoverlay_cleanup(void);
void axoverlay_redraw(GError**);
H
cc -std=gnu11 -O2 -Istubs $(pkg-config --cflags glib-2.0 cairo) overlay_test.c $(pkg-config --libs glib-2.0 cairo) -lm -o overlay_test
./overlay_test | tee result.txt
! grep -q FAIL result.txt
rm -rf stubs overlay_test result.txt overlay_2faces.png
