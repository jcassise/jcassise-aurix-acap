/* AURIX - bounded offline queue of events and their images (thread-safe). Upserts by eventId: a newer
 * revision replaces the stored body; when full, the lowest priority (then oldest) is dropped first. */
#ifndef AURIX_EVENT_QUEUE_H
#define AURIX_EVENT_QUEUE_H
#include <stddef.h>

#define EQ_MAX_ITEMS 400
#define EQ_MAX_IMAGE_BYTES (24u << 20)

typedef struct {
    char event_id[37];
    int revision, sent_revision, priority;
    long long created_ms;
    char *json;
    unsigned char *face, *scene;
    size_t face_len, scene_len;
    int face_sent, scene_sent;
} eq_item;

typedef struct {
    int events_pending, images_pending;
    long long oldest_ms;
    unsigned dropped;
} eq_stats;

/* Takes ownership of json/face/scene (NULL = keep what is stored). */
void eq_push(const char *event_id, int revision, int priority, long long now_ms, char *json,
             unsigned char *face, size_t face_len, unsigned char *scene, size_t scene_len);

/* Uploader access: copies out the oldest item with work to do (caller frees the copy with eq_item_free). */
int  eq_take(eq_item *out);
void eq_item_free(eq_item *it);
void eq_mark(const char *event_id, int sent_revision, int face_sent, int scene_sent);   /* -1 = unchanged */

eq_stats eq_get_stats(void);
void eq_reset(void);

#endif
