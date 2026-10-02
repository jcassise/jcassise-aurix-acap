#include "infer.h"
#include <fcntl.h>
#include <larod.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <syslog.h>
#include <unistd.h>

struct aurix_model {
    larodConnection *conn;
    larodModel *model;
    larodTensor **in, **out;
    size_t nin, nout;
    void **in_map, **out_map;
    size_t *in_sz, *out_sz;
    larodJobRequest *req;
};

static void log_err(const char *what, larodError **e)
{
    syslog(LOG_ERR, "larod %s: %s", what, (e && *e && (*e)->msg) ? (*e)->msg : "unknown error");
    larodClearError(e);
}

static int map_tensors(larodTensor **t, size_t n, void ***maps, size_t **sizes)
{
    *maps = calloc(n, sizeof(void *));
    *sizes = calloc(n, sizeof(size_t));
    if (!*maps || !*sizes) return -1;
    for (size_t i = 0; i < n; i++) {
        larodError *e = NULL;
        int fd = -1;
        size_t sz = 0;
        if (!larodGetTensorFd(t[i], &fd, &e)) { log_err("get fd", &e); return -1; }
        if (!larodGetTensorFdSize(t[i], &sz, &e)) { log_err("get fd size", &e); return -1; }
        void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) { syslog(LOG_ERR, "mmap tensor %zu failed", i); return -1; }
        (*maps)[i] = p;
        (*sizes)[i] = sz;
    }
    return 0;
}

static void unmap_tensors(void **maps, size_t *sizes, size_t n)
{
    if (!maps) return;
    for (size_t i = 0; i < n; i++)
        if (maps[i]) munmap(maps[i], sizes[i]);
    free(maps);
    free(sizes);
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
    m->model = larodLoadModel(m->conn, fd, dev, LAROD_ACCESS_PRIVATE, path, NULL, &e);
    if (!m->model) { log_err("load model", &e); goto fail; }

    m->in = larodAllocModelInputs(m->conn, m->model, LAROD_FD_PROP_MAP, &m->nin, NULL, &e);
    if (!m->in) { log_err("alloc inputs", &e); goto fail; }
    m->out = larodAllocModelOutputs(m->conn, m->model, LAROD_FD_PROP_MAP, &m->nout, NULL, &e);
    if (!m->out) { log_err("alloc outputs", &e); goto fail; }
    if (map_tensors(m->in, m->nin, &m->in_map, &m->in_sz) ||
        map_tensors(m->out, m->nout, &m->out_map, &m->out_sz))
        goto fail;

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
    unmap_tensors(m->in_map, m->in_sz, m->nin);
    unmap_tensors(m->out_map, m->out_sz, m->nout);
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
    if (bytes) *bytes = m->in_sz[i];
    return m->in_map[i];
}

const void *model_output(aurix_model *m, size_t i, size_t *bytes)
{
    if (i >= m->nout) return NULL;
    if (bytes) *bytes = m->out_sz[i];
    return m->out_map[i];
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

int model_input_dims(aurix_model *m, size_t i, int *h, int *w, int *c)
{
    if (i >= m->nin) return -1;
    larodError *e = NULL;
    const larodTensorDims *d = larodGetTensorDims(m->in[i], &e);
    if (!d) { log_err("get dims", &e); return -1; }
    if (d->len != 4) return -1;
    *h = (int)d->dims[1];
    *w = (int)d->dims[2];
    *c = (int)d->dims[3];
    return 0;
}

int model_run(aurix_model *m)
{
    larodError *e = NULL;
    if (!larodRunJob(m->conn, m->req, &e)) { log_err("run job", &e); return -1; }
    return 0;
}
