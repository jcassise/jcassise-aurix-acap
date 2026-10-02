#include "tensor.h"
#include <stdint.h>
#include <string.h>

#define TENSOR_MAX_DIMS 8

int tensor_is_padded(size_t ndims, const size_t *dims, const size_t *pitches, size_t elem)
{
    size_t expect = elem;
    for (size_t k = ndims; k-- > 0;) {
        expect *= dims[k];
        if (pitches[k] != expect) return 1;
    }
    return 0;
}

void tensor_repack(void *dst, const void *src, size_t L, const size_t *dims,
                   const size_t *pitches, size_t elem)
{
    if (L == 0 || L > TENSOR_MAX_DIMS) return;
    const size_t row = dims[L - 1] * elem;
    size_t rows = 1, idx[TENSOR_MAX_DIMS] = { 0 };
    for (size_t k = 0; k + 1 < L; k++) rows *= dims[k];
    uint8_t *d = dst;
    for (size_t r = 0; r < rows; r++) {
        size_t off = 0;
        for (size_t k = 0; k + 1 < L; k++) off += idx[k] * pitches[k + 1];
        memcpy(d + r * row, (const uint8_t *)src + off, row);
        for (size_t k = L - 1; k-- > 0;) {
            if (++idx[k] < dims[k]) break;
            idx[k] = 0;
        }
    }
}
