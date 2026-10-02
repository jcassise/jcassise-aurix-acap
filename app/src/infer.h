/* AURIX - thin larod v3 wrapper: one model, mmapped input/output tensors, reusable job. */
#ifndef AURIX_INFER_H
#define AURIX_INFER_H
#include <stddef.h>

typedef struct aurix_model aurix_model;

typedef enum { AURIX_DT_UNKNOWN = 0, AURIX_DT_UINT8, AURIX_DT_INT8, AURIX_DT_FLOAT32 } aurix_dtype;

/* path: .tflite model. device: e.g. "cpu-tflite", "axis-a8-dlpu-tflite". Returns NULL on failure. */
aurix_model *model_load(const char *path, const char *device);
void model_free(aurix_model *m);

size_t model_num_inputs(const aurix_model *m);
size_t model_num_outputs(const aurix_model *m);
void *model_input(aurix_model *m, size_t i, size_t *bytes);
const void *model_output(aurix_model *m, size_t i, size_t *bytes);
aurix_dtype model_input_type(aurix_model *m, size_t i);
aurix_dtype model_output_type(aurix_model *m, size_t i);

/* Input dims assuming NHWC [1,H,W,C]. Returns 0 on success. */
int model_input_dims(aurix_model *m, size_t i, int *h, int *w, int *c);

int model_run(aurix_model *m);

#endif
