/*
 * Just enough flattened-device-tree parsing to read what the bootloader left in
 * /chosen: the initrd location and the bootloader's own command line (which
 * names the DSI panel). Big-endian, 4-byte aligned tokens.
 *
 * The tree is bootloader input and is treated as untrusted: the header's
 * totalsize bounds every offset and every string is required to be
 * NUL-terminated inside its own block before it is compared or copied. This
 * runs with the MMU off, before any mapping of arbitrary memory.
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

/* A well-formed tree needs a handful of tokens per node. A truncated one can
 * loop inside totalsize forever, so give up after this many. */
#define FDT_MAX_TOKENS	100000u

/*
 * Byte-wise big-endian load. It must not be a plain u32 load: with the MMU off
 * everything is Device memory and an unaligned u32 access faults. The FDT the
 * bootloader hands us need not be 4-byte aligned -- in an Image.gz-dtb the
 * DTBs are an unpadded cat, so member #2 of the stock starqltechn set starts
 * at 0xb5c9d.
 */
static uint32_t be32(uintptr_t a)
{
	const volatile uint8_t *p = (const volatile uint8_t *)a;

	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	       (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

/* Length of the NUL-terminated string at p, or -1 if it runs past end. */
static long str_len(uintptr_t p, uintptr_t end)
{
	for (uintptr_t q = p; q < end; q++)
		if (!*(volatile char *)q)
			return (long)(q - p);
	return -1;
}

/* Byte-wise compare of the in-tree string at [p, end) against want. */
static int str_eq(uintptr_t p, uintptr_t end, const char *want)
{
	long n = str_len(p, end);

	if (n < 0)
		return 0;
	for (long i = 0; i < n; i++)
		if (*(volatile char *)(p + i) != *want++)
			return 0;
	return *want == 0;
}

/*
 * Round an OFFSET inside the tree (not an address) up to 4 bytes. The FDT base
 * itself may be unaligned, so aligning the absolute address would be wrong.
 */
static uint32_t align4(uint32_t off)
{
	return (off + 3) & ~(uint32_t)3;
}

/*
 * Find property `prop` of the /chosen node. On success *val is the property
 * value and *len its raw length, both inside the tree. Returns 0 if the tree
 * is malformed, the node or the property is absent, or the property lies
 * outside the tree.
 */
static int chosen_prop(uintptr_t fdt, const char *prop, uintptr_t *val,
		       uint32_t *len)
{
	uint32_t total, off_struct, off_strings, sz_strings;
	uintptr_t end, strings, strings_end;
	uint32_t p;			/* offset into the tree, not an address */
	unsigned tokens = 0;
	int depth = 0, in_chosen = 0;

	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	total = be32(fdt + 4);
	if (total < 40)
		return 0;
	end = fdt + total;
	off_struct = be32(fdt + 8);		/* off_dt_struct */
	off_strings = be32(fdt + 12);		/* off_dt_strings */
	sz_strings = be32(fdt + 32);		/* size_dt_strings */
	if (off_struct >= total || off_strings >= total ||
	    sz_strings > total - off_strings)
		return 0;
	p = off_struct;
	strings = fdt + off_strings;
	strings_end = strings + sz_strings;

	for (;;) {
		uint32_t tok;
		long n;

		if (p + 4 > total || ++tokens > FDT_MAX_TOKENS)
			return 0;
		tok = be32(fdt + p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE:
			if ((n = str_len(fdt + p, end)) < 0)
				return 0;
			depth++;
			in_chosen = depth == 2 && str_eq(fdt + p, end, "chosen");
			p = align4(p + (uint32_t)n + 1);
			break;
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t plen, nameoff;

			if (p + 8 > total)
				return 0;
			plen = be32(fdt + p);
			nameoff = be32(fdt + p + 4);
			/* value must lie inside the tree */
			if (plen > total - (p + 8))
				return 0;
			if (in_chosen && nameoff < sz_strings &&
			    str_eq(strings + nameoff, strings_end, prop)) {
				*val = fdt + p + 8;
				*len = plen;
				return 1;
			}
			p = align4(p + 8 + plen);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	uintptr_t val;
	uint32_t len;

	if (!chosen_prop(fdt, prop, &val, &len))
		return 0;
	if (len == 4)
		*out = be32(val);
	else if (len == 8)
		*out = (uint64_t)be32(val) << 32 | be32(val + 4);
	else
		return 0;
	return 1;
}

int fdt_chosen_str(uintptr_t fdt, const char *prop, char *buf, unsigned buflen)
{
	uintptr_t val;
	uint32_t len;

	if (!buf || !buflen)
		return 0;
	buf[0] = 0;
	if (!chosen_prop(fdt, prop, &val, &len) || !len)
		return 0;
	/* Copy as much as fits and always terminate. A caller that needs the whole
	 * string has to size the buffer for it; a bootloader command line can be
	 * longer than any sane stack buffer, and refusing outright would turn a
	 * long command line into "property absent". */
	if (len > buflen - 1)
		len = buflen - 1;
	for (uint32_t i = 0; i < len; i++)
		buf[i] = *(volatile char *)(val + i);
	buf[len] = 0;
	return 1;
}
