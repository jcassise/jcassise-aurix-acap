#include "event_queue.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static eq_item q[EQ_MAX_ITEMS];
static int n;
static size_t image_bytes;
static unsigned dropped;

static void release(eq_item *it)
{
    image_bytes -= it->face_len + it->scene_len;
    free(it->json);
    free(it->face);
    free(it->scene);
    memset(it, 0, sizeof *it);
}

static void remove_at(int i)
{
    release(&q[i]);
    memmove(&q[i], &q[i + 1], sizeof q[0] * (size_t)(n - i - 1));
    n--;
    memset(&q[n], 0, sizeof q[0]);
}

static int done(const eq_item *it)
{
    return it->sent_revision >= it->revision && (!it->face || it->face_sent) && (!it->scene || it->scene_sent);
}

/* lowest priority first, then oldest; never the item being inserted */
static int victim(const char *keep)
{
    int v = -1;
    for (int i = 0; i < n; i++) {
        if (!strcmp(q[i].event_id, keep)) continue;
        if (v < 0 || q[i].priority < q[v].priority || (q[i].priority == q[v].priority && q[i].created_ms < q[v].created_ms))
            v = i;
    }
    return v;
}

void eq_push(const char *id, int revision, int priority, long long now, char *json,
             unsigned char *face, size_t face_len, unsigned char *scene, size_t scene_len)
{
    pthread_mutex_lock(&mu);
    int i = 0;
    while (i < n && strcmp(q[i].event_id, id)) i++;
    if (i == n) {
        if (n == EQ_MAX_ITEMS) {
            int v = victim(id);
            if (v >= 0) { remove_at(v); dropped++; }
            i = n;
        }
        if (n < EQ_MAX_ITEMS) {
            memset(&q[n], 0, sizeof q[n]);
            strncpy(q[n].event_id, id, sizeof q[n].event_id - 1);
            q[n].created_ms = now;
            q[n].sent_revision = 0;
            i = n++;
        } else {                                       /* could not make room */
            free(json); free(face); free(scene); dropped++;
            pthread_mutex_unlock(&mu);
            return;
        }
    }
    eq_item *it = &q[i];
    if (priority > it->priority) it->priority = priority;
    if (json && revision > it->revision) { free(it->json); it->json = json; it->revision = revision; }
    else free(json);
    if (face) { image_bytes -= it->face_len; free(it->face); it->face = face; it->face_len = face_len; it->face_sent = 0; image_bytes += face_len; }
    if (scene) { image_bytes -= it->scene_len; free(it->scene); it->scene = scene; it->scene_len = scene_len; it->scene_sent = 0; image_bytes += scene_len; }
    while (image_bytes > EQ_MAX_IMAGE_BYTES) {           /* images over budget: drop images of the least important */
        int v = victim(id);
        if (v < 0) break;
        remove_at(v);
        dropped++;
    }
    pthread_mutex_unlock(&mu);
}

int eq_take(eq_item *out)
{
    pthread_mutex_lock(&mu);
    for (int i = 0; i < n; i++) {
        if (done(&q[i])) continue;
        *out = q[i];
        out->json = q[i].json ? strdup(q[i].json) : NULL;
        out->face = NULL; out->scene = NULL;
        if (q[i].face && !q[i].face_sent) { out->face = malloc(q[i].face_len); if (out->face) memcpy(out->face, q[i].face, q[i].face_len); }
        if (q[i].scene && !q[i].scene_sent) { out->scene = malloc(q[i].scene_len); if (out->scene) memcpy(out->scene, q[i].scene, q[i].scene_len); }
        pthread_mutex_unlock(&mu);
        return 1;
    }
    pthread_mutex_unlock(&mu);
    return 0;
}

void eq_item_free(eq_item *it)
{
    free(it->json);
    free(it->face);
    free(it->scene);
    memset(it, 0, sizeof *it);
}

void eq_mark(const char *id, int sent_rev, int face_sent, int scene_sent)
{
    pthread_mutex_lock(&mu);
    for (int i = 0; i < n; i++) {
        if (strcmp(q[i].event_id, id)) continue;
        if (sent_rev > q[i].sent_revision) q[i].sent_revision = sent_rev;
        if (face_sent > 0) q[i].face_sent = 1;
        if (scene_sent > 0) q[i].scene_sent = 1;
        if (done(&q[i])) remove_at(i);
        break;
    }
    pthread_mutex_unlock(&mu);
}

eq_stats eq_get_stats(void)
{
    eq_stats s = { 0, 0, 0, dropped };
    pthread_mutex_lock(&mu);
    for (int i = 0; i < n; i++) {
        if (q[i].sent_revision < q[i].revision) s.events_pending++;
        s.images_pending += (q[i].face && !q[i].face_sent) + (q[i].scene && !q[i].scene_sent);
        if (!s.oldest_ms || q[i].created_ms < s.oldest_ms) s.oldest_ms = q[i].created_ms;
    }
    s.dropped = dropped;
    pthread_mutex_unlock(&mu);
    return s;
}

void eq_reset(void)
{
    pthread_mutex_lock(&mu);
    while (n) remove_at(n - 1);
    dropped = 0;
    image_bytes = 0;
    pthread_mutex_unlock(&mu);
}
