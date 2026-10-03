/* AURIX - how many people this camera can hold: memory, storage and matching speed (pure C). */
#ifndef AURIX_CAPACITY_H
#define AURIX_CAPACITY_H
#include <stdint.h>

/* Per-person cost assumptions (documented on the dashboard). */
#define CAP_RAM_BYTES_PER_PERSON   1024u      /* template + name + Pharos record, rounded up */
#define CAP_FLASH_BYTES_PER_PERSON (8u * 1024) /* cached aligned face crop + template + record (Pharos sync) */
#define CAP_RAM_RESERVE_KB         (96u * 1024)  /* leave for the OS, models and video buffers */
#define CAP_FLASH_RESERVE_KB       (32u * 1024)  /* leave for logs, queue and config */
#define CAP_MATCH_BUDGET_MS        5.0        /* matching time allowed per face */

typedef enum { CAP_BY_MEMORY = 0, CAP_BY_STORAGE, CAP_BY_MATCHING, CAP_UNKNOWN } cap_limit;

typedef struct {
    uint64_t by_memory, by_storage, by_matching;   /* 0 = unknown */
    uint64_t estimate;                             /* min of the known limits */
    cap_limit limited_by;
    double match_ms_at_estimate;                   /* per face, at `estimate` people */
} cap_result;

/* ns_per_entry: measured cost of comparing one face with one gallery entry (0 = unknown). */
cap_result capacity_estimate(uint64_t mem_available_kb, uint64_t storage_free_kb, double ns_per_entry);
const char *capacity_limit_name(cap_limit l);

#endif
