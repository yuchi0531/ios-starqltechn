#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/*
 * Copy the string property `prop` of /chosen into buf (NUL-terminated). Returns
 * 0 if the property is absent, not a string, or would not fit in buflen.
 */
int fdt_chosen_str(uintptr_t fdt, const char *prop, char *buf, unsigned buflen);
