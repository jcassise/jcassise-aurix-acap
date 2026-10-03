#include "capacity.h"

const char *capacity_limit_name(cap_limit l)
{
    static const char *n[] = { "memory", "storage", "matching speed", "unknown" };
    return n[l];
}

cap_result capacity_estimate(uint64_t mem_kb, uint64_t flash_kb, double ns_per_entry)
{
    cap_result r = { 0, 0, 0, 0, CAP_UNKNOWN, 0 };
    if (mem_kb > CAP_RAM_RESERVE_KB) r.by_memory = (mem_kb - CAP_RAM_RESERVE_KB) * 1024u / CAP_RAM_BYTES_PER_PERSON;
    else if (mem_kb) r.by_memory = 0;
    if (flash_kb > CAP_FLASH_RESERVE_KB) r.by_storage = (flash_kb - CAP_FLASH_RESERVE_KB) * 1024u / CAP_FLASH_BYTES_PER_PERSON;
    if (ns_per_entry > 0) r.by_matching = (uint64_t)(CAP_MATCH_BUDGET_MS * 1e6 / ns_per_entry);

    const uint64_t lim[3] = { r.by_memory, r.by_storage, r.by_matching };
    for (int i = 0; i < 3; i++) {
        if (!lim[i]) continue;
        if (r.limited_by == CAP_UNKNOWN || lim[i] < r.estimate) { r.estimate = lim[i]; r.limited_by = (cap_limit)i; }
    }
    if (ns_per_entry > 0) r.match_ms_at_estimate = r.estimate * ns_per_entry / 1e6;
    return r;
}
