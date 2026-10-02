#include "infer.h"
#include "tensor.h"
#include <fcntl.h>
#include <larod.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <syslog.h>
#include <unistd.h>

#define POWER_RETRIES 50          /* DLPU may be busy powering up; Axis examples retry too */
#define MAX_DIMS 8

typedef struct {
    larodTensor *t;
    void *map;
    size_t map_size;
    size_t ndims, dims[MAX_DIMS], pitches[MAX_DIMS];
    size_t elem;                  /* bytes per element */
    size_t packed_size;
    int padded;
    void *scratch;                /* packed copy for padded outputs */
} tensor_info;

struct aurix_model {
    larodConnection *conn;
    larodModel *model;
    larodTensor **in, **out;
    size_t nin, nout;
    tensor_info *tin, *tout;
    larodJobRequest *req;
};

static void log_err(const char *what, larodError **e)
{
    syslog(LOG_ERR, "larod %s: %s", what, (e && *e && (*e)->msg) ? (*e)->msg : "unknown error");
    larodClearError(e);
}

#ifdef AURIX_LEGACY_SDK
/* SDK 1.15 (AXIS OS 11.x, ARTPEC-7 CPU only): no DLPU power management. */
static int is_power_wait(const larodError *e) { (void)e; return 0; }
#else
static int is_power_wait(const larodError *e) { return e && e->code == LAROD_ERROR_POWER_NOT_AVAILABLE; }
#endif

static size_t elem_size(larodTensorDataType dt)
{
    switch (dt) {
    case LAROD_TENSOR_DATA_TYPE_UINT8:
    case LAROD_TENSOR_DATA_TYPE_INT8: return 1;
    case LAROD_TENSOR_DATA_TYPE_FLOAT32: return 4;
    default: return 0;
    }
}

static int describe(larodTensor **t, size_t n, tensor_info **out)
{
    *out = calloc(n, sizeof(tensor_info));
    if (!*out) return -1;
    for (size_t i = 0; i < n; i++) {
        tensor_info *ti = &(*out)[i];
        larodError *e = NULL;
        ti->t = t[i];

        int fd = larodGetTensorFd(t[i], &e);
        if (fd < 0) { log_err("get fd", &e); return -1; }
        if (!larodGetTensorFdSize(t[i], &ti->map_size, &e)) { log_err("get fd size", &e); return -1; }
        ti->map = mmap(NULL, ti->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (ti->map == MAP_FAILED) { ti->map = NULL; syslog(LOG_ERR, "mmap tensor %zu failed", i); return -1; }

        larodTensorDataType dt = larodGetTensorDataType(t[i], &e);
        if (dt == LAROD_TENSOR_DATA_TYPE_INVALID) { log_err("get data type", &e); return -1; }
        ti->elem = elem_size(dt);
        if (!ti->elem) { syslog(LOG_ERR, "tensor %zu: unsupported data type %d", i, (int)dt); return -1; }

        const larodTensorDims *d = larodGetTensorDims(t[i], &e);
        if (!d) { log_err("get dims", &e); return -1; }
        const larodTensorPitches *p = larodGetTensorPitches(t[i], &e);
        if (!p) { log_err("get pitches", &e); return -1; }
        if (d->len == 0 || d->len > MAX_DIMS || p->len != d->len) {
            syslog(LOG_ERR, "tensor %zu: unexpected dims/pitches rank", i);
            return -1;
        }
        ti->ndims = d->len;
        ti->packed_size = ti->elem;
        for (size_t k = 0; k < d->len; k++) {
            ti->dims[k] = d->dims[k];
            ti->pitches[k] = p->pitches[k];
            ti->packed_size *= d->dims[k];
        }
        ti->padded = tensor_is_padded(ti->ndims, ti->dims, ti->pitches, ti->elem);
        if (ti->map_size < ti->pitches[0]) {
            syslog(LOG_ERR, "tensor %zu: buffer smaller than pitch", i);
            return -1;
        }
    }
    return 0;
}

static void release(tensor_info *ti, size_t n)
{
    if (!ti) return;
    for (size_t i = 0; i < n; i++) {
        if (ti[i].map) munmap(ti[i].map, ti[i].map_size);
        free(ti[i].scratch);
    }
    free(ti);
}

aurix_model *model_load(const char *path, const char *device)
{
    larodError *e = NULL;
    aurix_model *m = calloc(1, sizeof(*m));
    if (!m) return NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0) { syslog(LOG_ERR, "cannot open model %s", path); free(m); return NULL; }

    if (!larodConnect(&m->conn, &e)) { log_err("connect", &e); goto fail; }
    const larodDevice *dev = larodGetDevice(m->conn, device, 0, &e);
    if (!dev) { log_err(device, &e); goto fail; }

    syslog(LOG_INFO, "loading %s on %s (can take minutes the first time)", path, device);
    for (int tries = 0; tries < POWER_RETRIES; tries++) {
        m->model = larodLoadModel(m->conn, fd, dev, LAROD_ACCESS_PRIVATE, "aurix", NULL, &e);
        if (m->model || !is_power_wait(e)) break;
        larodClearError(&e);
        usleep(250 * 1000 * (useconds_t)(tries + 1 < 8 ? tries + 1 : 8));
    }
    if (!m->model) { log_err("load model", &e); goto fail; }

    const uint32_t props = LAROD_FD_PROP_READWRITE | LAROD_FD_PROP_MAP;
    m->in = larodAllocModelInputs(m->conn, m->model, props, &m->nin, NULL, &e);
    if (!m->in) { log_err("alloc inputs", &e); goto fail; }
    m->out = larodAllocModelOutputs(m->conn, m->model, props, &m->nout, NULL, &e);
    if (!m->out) { log_err("alloc outputs", &e); goto fail; }
    if (describe(m->in, m->nin, &m->tin) || describe(m->out, m->nout, &m->tout)) goto fail;

    for (size_t i = 0; i < m->nout; i++) {
        if (m->tout[i].padded) {
            m->tout[i].scratch = malloc(m->tout[i].packed_size);
            if (!m->tout[i].scratch) goto fail;
            syslog(LOG_INFO, "output %zu is padded; repacking on read", i);
        }
    }

    m->req = larodCreateJobRequest(m->model, m->in, m->nin, m->out, m->nout, NULL, &e);
    if (!m->req) { log_err("create job", &e); goto fail; }

    close(fd);
    syslog(LOG_INFO, "loaded %s on %s (%zu in, %zu out)", path, device, m->nin, m->nout);
    return m;
fail:
    close(fd);
    model_free(m);
    return NULL;
}

void model_free(aurix_model *m)
{
    if (!m) return;
    larodError *e = NULL;
    larodDestroyJobRequest(&m->req);
    release(m->tin, m->nin);
    release(m->tout, m->nout);
    if (m->in && !larodDestroyTensors(m->conn, &m->in, m->nin, &e)) log_err("destroy inputs", &e);
    if (m->out && !larodDestroyTensors(m->conn, &m->out, m->nout, &e)) log_err("destroy outputs", &e);
    larodDestroyModel(&m->model);
    if (m->conn && !larodDisconnect(&m->conn, &e)) log_err("disconnect", &e);
    free(m);
}

size_t model_num_inputs(const aurix_model *m) { return m->nin; }
size_t model_num_outputs(const aurix_model *m) { return m->nout; }

void *model_input(aurix_model *m, size_t i, size_t *bytes)
{
    if (i >= m->nin) return NULL;
    if (bytes) *bytes = m->tin[i].map_size;
    return m->tin[i].map;
}

int model_input_dims(aurix_model *m, size_t i, int *h, int *w, int *c, size_t *row_pitch)
{
    if (i >= m->nin || m->tin[i].ndims != 4) return -1;
    const tensor_info *t = &m->tin[i];
    *h = (int)t->dims[1];
    *w = (int)t->dims[2];
    *c = (int)t->dims[3];
    if (t->pitches[3] != t->dims[3] * t->elem) {
        syslog(LOG_ERR, "input %zu: padded pixels not supported", i);
        return -1;
    }
    if (row_pitch) *row_pitch = t->pitches[2];
    return 0;
}

const void *model_output(aurix_model *m, size_t i, size_t *bytes)
{
    if (i >= m->nout) return NULL;
    tensor_info *t = &m->tout[i];
    if (bytes) *bytes = t->packed_size;
    if (!t->padded) return t->map;
    tensor_repack(t->scratch, t->map, t->ndims, t->dims, t->pitches, t->elem);
    return t->scratch;
}

static aurix_dtype map_dtype(const larodTensor *t)
{
    larodError *e = NULL;
    larodTensorDataType dt = larodGetTensorDataType(t, &e);
    larodClearError(&e);
    switch (dt) {
    case LAROD_TENSOR_DATA_TYPE_UINT8: return AURIX_DT_UINT8;
    case LAROD_TENSOR_DATA_TYPE_INT8: return AURIX_DT_INT8;
    case LAROD_TENSOR_DATA_TYPE_FLOAT32: return AURIX_DT_FLOAT32;
    default: return AURIX_DT_UNKNOWN;
    }
}

aurix_dtype model_input_type(aurix_model *m, size_t i) { return i < m->nin ? map_dtype(m->in[i]) : AURIX_DT_UNKNOWN; }
aurix_dtype model_output_type(aurix_model *m, size_t i) { return i < m->nout ? map_dtype(m->out[i]) : AURIX_DT_UNKNOWN; }

int model_run(aurix_model *m)
{
    larodError *e = NULL;
    for (int tries = 0; tries < POWER_RETRIES; tries++) {
        if (larodRunJob(m->conn, m->req, &e)) return 0;
        if (!is_power_wait(e)) break;
        larodClearError(&e);
        usleep(20 * 1000);
    }
    log_err("run job", &e);
    return -1;
}
