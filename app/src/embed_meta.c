#include "embed_meta.h"
#include <stdio.h>
#include <string.h>

int embed_meta_load(const char *path, embed_meta *m)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    memset(m, 0, sizeof(*m));
    int got_in = 0, got_out = 0;
    char line[256], type[16];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "input %d %d", &m->in_w, &m->in_h) == 2) got_in = 1;
        else if (sscanf(line, "output %d %15s %f %d", &m->dim, type, &m->scale, &m->zero_point) == 4)
            got_out = !strcmp(type, "int8");
    }
    fclose(f);
    return (got_in && got_out && m->dim > 0 && m->in_w > 0 && m->in_h > 0) ? 0 : -1;
}
