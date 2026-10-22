#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/*
 * Copy the string property `prop` of /chosen into buf (NUL-terminated). Returns
 * 0 if the property is absent or not a string. A value longer than buflen - 1
 * is truncated and still terminated (never an overrun, and never reported as
 * "absent"), so a caller that needs the whole line must size the buffer for it:
 * ABL's merged /chosen/bootargs is ~1793 bytes on starqltechn.
 */
int fdt_chosen_str(uintptr_t fdt, const char *prop, char *buf, unsigned buflen);
