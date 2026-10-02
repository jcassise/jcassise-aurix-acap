/* AURIX - pitched tensor helpers (pure C, host-testable). */
#ifndef AURIX_TENSOR_H
#define AURIX_TENSOR_H
#include <stddef.h>

/* pitches[k] = bytes spanned by dims[k..] (larod convention); stride of dim k is pitches[k+1]
 * (innermost elements are contiguous). Returns 1 if the layout has any padding. */
int tensor_is_padded(size_t ndims, const size_t *dims, const size_t *pitches, size_t elem);

/* Copy a pitched tensor into dst in packed order. dst must hold prod(dims)*elem bytes. */
void tensor_repack(void *dst, const void *src, size_t ndims, const size_t *dims,
                   const size_t *pitches, size_t elem);

#endif
