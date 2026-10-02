/* AURIX - thin larod v3 wrapper: one model, mmapped input/output tensors, reusable job.
 * Tensors may be padded (larod "pitches", common on the DLPU): inputs expose their row
 * pitch; outputs are handed out tightly packed (repacked into scratch only if padded). */
#ifndef AURIX_INFER_H
#define AURIX_INFER_H
#include <stddef.h>

typedef struct aurix_model aurix_model;

typedef enum { AURIX_DT_UNKNOWN = 0, AURIX_DT_UINT8, AURIX_DT_INT8, AURIX_DT_FLOAT32 } aurix_dtype;

/* path: .tflite model. device: e.g. "cpu-tflite", "axis-a8-dlpu-tflite". Returns NULL on failure.
 * Loading on the DLPU can take minutes the first time. */
aurix_model *model_load(const char *path, const char *device);
void model_free(aurix_model *m);

size_t model_num_inputs(const aurix_model *m);
size_t model_num_outputs(const aurix_model *m);

/* Raw mapped input buffer; write rows at model_input_row_pitch() spacing. */
void *model_input(aurix_model *m, size_t i, size_t *bytes);
/* Input dims assuming NHWC [1,H,W,C], plus row pitch in bytes. Fails if pixels are padded. */
int model_input_dims(aurix_model *m, size_t i, int *h, int *w, int *c, size_t *row_pitch);

/* Output data, tightly packed (valid until the next model_run). bytes = packed size. */
const void *model_output(aurix_model *m, size_t i, size_t *bytes);

aurix_dtype model_input_type(aurix_model *m, size_t i);
aurix_dtype model_output_type(aurix_model *m, size_t i);

int model_run(aurix_model *m);

#endif
