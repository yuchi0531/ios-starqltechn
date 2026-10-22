/* SPDX-License-Identifier: GPL-2.0-only
 * SDM845 / DSI0 command-path transmit for starqltechn (Galaxy S9).
 *
 * ABL hands over a live link: MDSS, DSI0, the PLL and the S6E3HA8 panel are
 * powered and PLL-locked. This file therefore does not initialise anything. It
 * only pushes MIPI DCS packets through the DSI command DMA, which is a separate
 * engine from the video engine, and it reads (never writes) every register that
 * belongs to timing, the PHY or the clock tree.
 *
 * Whether the panel is in video or command mode is NOT assumed: both S9 panels
 * are declared dsi_cmd_mode in the vendor DTS (dtsi_px01.dtsi:70,
 * dtsi_mq01.dtsi:66), which contradicts what an earlier version of this file
 * and NOTES.md said. DSI_CTRL's ENABLE / VIDEO_MODE_EN / CMD_MODE_EN bits are
 * the deciding evidence, so starq_dsi_init() logs them and every DMA-scheduling
 * decision below reads the register instead of the DT.
 *
 * ---- register file ----
 * The offsets below are the vendor techpack's absolute offsets
 * (techpack/display/msm/dsi/dsi_ctrl_reg.h in the Samsung kernel; the
 * extracted tree has it at drivers/gpu/drm/msm/dsi-staging/dsi_ctrl_reg.h).
 * They equal mainline drivers/gpu/drm/msm/dsi/dsi.xml.h offsets plus
 * DSI_6G_REG_SHIFT (4) from dsi_cfg.h, because for DSI6G and later "all other
 * registers are 4-byte shifted down" behind the 6G_HW_VERSION register at 0
 * (dsi_host.c:dsi_get_version()):
 *
 *   DSI_CTRL                    vendor 0x004   mainline 0x000 + 4
 *   DSI_STATUS                  vendor 0x008   mainline 0x004 + 4
 *   DSI_VIDEO_MODE_CTRL         vendor 0x010   mainline 0x00c + 4
 *   DSI_VIDEO_MODE_ACTIVE_V     vendor 0x028   mainline 0x024 + 4
 *   DSI_COMMAND_MODE_DMA_CTRL   vendor 0x03c   mainline 0x038 + 4
 *   DSI_DMA_CMD_OFFSET          vendor 0x048   mainline 0x044 + 4
 *   DSI_DMA_CMD_LENGTH          vendor 0x04c   mainline 0x048 + 4
 *   DSI_DMA_FIFO_CTRL           vendor 0x050   mainline 0x04c + 4
 *   DSI_TRIG_CTRL               vendor 0x084   mainline 0x080 + 4
 *   DSI_CMD_MODE_DMA_SW_TRIGGER vendor 0x090   mainline 0x08c + 4
 *   DSI_INT_CTRL                vendor 0x110   mainline 0x10c + 4
 *   DSI_CLK_CTRL                vendor 0x11c   mainline 0x118 + 4
 *
 * ---- packet layout in memory ----
 * Both implementations agree byte for byte even though they disagree about
 * which element of struct mipi_dsi_packet holds what (mainline: header[0] =
 * data type, header[1..2] = word count; the struct has no padding):
 *
 *   [0] word count low / short-write parameter 0
 *   [1] word count high / short-write parameter 1
 *   [2] data type bits 5:0, virtual channel bits 7:6
 *   [3] bit7 last packet, bit6 long packet, bit5 embedded BTA (reads)
 *   [4..] payload, padded to a 4-byte multiple with 0xff
 *
 * techpack dsi_ctrl_copy_and_pad_cmd() copies header[0..3] verbatim and sets
 * buf[3] |= BIT(6) for a long packet; mainline dsi_cmd_dma_add() writes
 * data[0]=header[1], data[1]=header[2], data[2]=header[0],
 * data[3]=BIT(7)|(BIT(6) if long) and pads with 0xff. Same bytes either way.
 * The word-count position is also where a short packet's two parameters go,
 * which is why a 1-byte DCS write is a short packet with only byte [0] used
 * (mipi_dsi_create_packet(): header[0] = tx[0], header[1] = tx_len > 1 ? tx[1] : 0).
 *
 * ---- the two paths to the DMA engine ----
 *   * Embedded mode (dsi_ctrl_hw_cmn_kickoff_command): the controller fetches
 *     the packet from memory at DSI_DMA_CMD_OFFSET. Every Android panel
 *     command takes this path, but there the offset is an IOVA, so in a
 *     bootloader with the MMU off it only works if the DSI SMMU is bypassed or
 *     identity-mapped over the framebuffer reservation. If it is not, the
 *     transfer faults and we time out.
 *   * FIFO mode (dsi_ctrl_hw_cmn_kickoff_fifo_command): the same packet bytes
 *     are pushed into the command test-pattern FIFO, so there is no memory
 *     access and no SMMU at all. The vendor uses this for secure sessions.
 *     Limited to DSI_CTRL_MAX_CMD_FIFO_STORE_SIZE (64) bytes.
 * We try the memory path first (the well-trodden one) and fall back to the
 * FIFO path on failure, logging which one worked.
 *
 * ---- video mode ----
 * mainline dsi_wait4video_eng_busy() waits for a VIDEO_DONE interrupt and then
 * sleeps 2-4 ms "to skip BLLP"; dsi_cmds2buf_tx() additionally refuses a
 * command longer than one pixel line (hdisplay*bpp/8 = 4320 bytes at 1440 px,
 * 24 bpp), since in video mode a command only goes out during BLLP.
 *
 * The techpack driver does something stronger, and this is what we implement
 * *when DSI_CTRL says video mode*: for controller version >= 2.2 (SDM845 is
 * "qcom,dsi-ctrl-hw-v2.2" -> DSI_CTRL_VERSION_2_2)
 * dsi_ctrl_wait_for_video_done() returns immediately *because*
 * dsi_ctrl_hw_22_schedule_dma_cmd() has programmed DSI_DMA_SCHEDULE_CTRL, so
 * the hardware emits the DMA command itself at the configured scan line.
 * dsi_message_tx() computes that line as v_back_porch + v_sync_width + v_active,
 * which is exactly the "active_v end" field of DSI_VIDEO_MODE_ACTIVE_V
 * (dsi_ctrl_hw_cmn_setup_video_mode(): active_v_end = v_sync_width +
 * v_back_porch + v_active). We therefore derive the line from that read-only
 * register: the first line of vertical blanking. Nothing is hardcoded from the
 * panel's DT timing. In command mode the trigger is the frame update and there
 * is nothing to dodge, so no schedule is programmed (neither does the vendor
 * driver program one when the panel is in command mode).
 *
 * starqdsisw=1 in the boot-args skips the schedule and uses the plain software
 * trigger, i.e. mainline's "send whenever the DMA engine is idle" behaviour,
 * for experiments.
 *
 * ---- the ELVSS payload ----
 * See dsi.h for the mechanism and its sources. In short: 0xB5 is NOT readable
 * as a page. The vendor DT exposes a 2-byte read at a 0xB0-selected offset
 * (samsung,elvss_rx_cmds_revA = [06 01 00 00 00 00 01 B5 02 16], i.e. rx_len=2,
 * read_startoffset 0x16 on PX01 / 0x17 on MQ01; dsi_panel.c:2113-2114). There
 * is therefore no read-modify-write here and no DCS read at all: only 2 bytes
 * are exposed, a paginated read would need three passes plus BTA handling, and
 * that is not worth the risk on an unproven link. A 4-byte 0xB5 write is the
 * correct universal move instead: it changes exactly the bytes the vendor
 * driver changes (1..3) and on PX01 leaves bytes 4..23 - including the
 * per-panel OTP black-level trim in byte 23 - untouched.
 *
 * If a read ever becomes necessary, DCS 0xD6 + a 0xB0 page select is the path
 * the vendor driver uses; that is future work, not this.
 */
#include "dsi.h"
#include "loader.h"

/* ---- controller registers (see the table above) ---- */
#define R_HW_VERSION		0x000
#define R_CTRL			0x004
#define R_STATUS		0x008
#define R_FIFO_STATUS		0x00c
#define R_VIDEO_MODE_CTRL	0x010
#define R_VIDEO_MODE_ACTIVE_V	0x028
#define R_VIDEO_MODE_TOTAL	0x02c
#define R_CMD_DMA_CTRL		0x03c
#define R_DMA_CMD_OFFSET	0x048
#define R_DMA_CMD_LENGTH	0x04c
#define R_DMA_FIFO_CTRL		0x050
#define R_ACK_ERR_STATUS	0x068
#define R_TRIG_CTRL		0x084
#define R_TRIG_DMA		0x090
#define R_TIMEOUT_STATUS	0x0c0
#define R_DMA_SCHED		0x100
#define R_ERR_INT_MASK0		0x10c
#define R_INT_CTRL		0x110
#define R_CLK_CTRL		0x11c
#define R_TPG_CTRL		0x15c
#define R_TPG_CMD_DMA_INIT	0x17c
#define R_TPG_DMA_FIFO_RESET	0x1ec

/* PHY registers, read-only (dsi_phy_hw_v3_0.c) */
#define P_CLK_CFG0		0x010
#define P_CLK_CFG1		0x014
#define P_VREG_CTRL		0x020
#define P_TIMING_CTRL_0		0x0ac
#define P_PHY_STATUS		0x0ec
#define P_LANE_STATUS0		0x0f4
#define P_LANE_STATUS1		0x0f8
#define P_GLBL_CTRL		0x018

#define CTRL_ENABLE		(1u << 0)
#define CTRL_VID_MODE_EN	(1u << 1)
#define CTRL_CMD_MODE_EN	(1u << 2)

#define STATUS_CMD_ENGINE_BUSY	(1u << 0)
#define STATUS_CMD_DMA_BUSY	(1u << 1)

/* DSI_COMMAND_MODE_DMA_CTRL, per dsi_ctrl_hw_cmn_kickoff_command() */
#define DMA_CTRL_BROADCAST	(1u << 31)
#define DMA_CTRL_IS_MASTER	(1u << 30)
#define DMA_CTRL_WC_SEL		(1u << 29)
#define DMA_CTRL_EMBEDDED	(1u << 28)
#define DMA_CTRL_LPM		(1u << 26)
#define DMA_CTRL_PKT_TYPE	(1u << 24)

#define INT_CTRL_DMA_DONE	(1u << 0)
#define INT_CTRL_ERROR		(1u << 24)

/* dsi_ctrl_hw_22_schedule_dma_cmd(): bit 28 enables, 16-bit line number. */
#define DMA_SCHED_EN		(1u << 28)

/* DSI_TEST_PATTERN_GEN_CTRL: CMD_DMA_TPG_EN | TPG_DMA_FIFO_MODE |
 * CMD_DMA_PATTERN_SEL = 3 (custom pattern from the TPG DMA FIFO). */
#define TPG_CMD_DMA_FIFO	((1u << 1) | (1u << 2) | (3u << 16))

/* MIPI DCS data types (include/drm/drm_mipi_dsi.h) */
#define DT_DCS_SHORT_WRITE	0x05
#define DT_DCS_SHORT_WRITE_PARAM 0x15
#define DT_DCS_LONG_WRITE	0x39

/* Packet flag bits in byte 3 */
#define PKT_LAST		(1u << 7)
#define PKT_LONG		(1u << 6)

/* DSI_EMBEDDED_MODE_DMA_MAX_SIZE_BYTES / DSI_CTRL_MAX_CMD_FIFO_STORE_SIZE */
#define DMA_MAX_BYTES		256u
#define FIFO_MAX_BYTES		64u

/* Bounded waits. There is no usable clock here (the bootloader may enter with
 * CNTFRQ_EL0 = 0, see start.S), so these are iteration counts, not
 * milliseconds: one iteration is one Device-memory register read. A 60 Hz
 * frame is ~16 ms, far inside these bounds; a transfer that has not completed
 * after this many reads is never going to, and the caller gets an error
 * instead of a hang. */
#define POLL_IDLE		50000u
#define POLL_DONE		200000u

static uintptr_t dsi_ctrl, dsi_phy;
static uintptr_t dsi_cmd_pa;	/* physical address the DMA engine reads */
static int dsi_inited;
static int dsi_sw_trigger;
static int dsi_link_down;	/* DSI_CTRL.ENABLE was clear: send nothing */

/* ---- the DMA-visible packet buffer ----
 * The MMU is off in the loader, so the address the DSI DMA reads is physical,
 * and we must not program the MDSS SMMU: the buffer has to sit in memory the
 * display pipeline already has mapped. That is the bootloader framebuffer
 * reservation ("framebuffer_mem", 0x9d400000 + 0x02400000, the only region
 * ABL mapped for MDSS). The visible panel is the first 1440*2960*4 =
 * 0x1042800 bytes, so STARQ_DSI_CMD_BUF_PA (the last page of the carve-out) is
 * outside anything the test pattern, the text console or XNU's framebuffer
 * console touches. Caches are off (start.S clears SCTLR.C before entry), so the
 * stores are already visible to the DMA engine. */
static uint8_t *cmd_buf(void)
{
#if defined(__aarch64__)
	return (uint8_t *)STARQ_DSI_CMD_BUF_PA;
#else
	return starq_dsi_cmd_buf;	/* host array, asserted on by host_test.c */
#endif
}

/* ---- MMIO ---- */
#if defined(__aarch64__)

#define dsb() __asm__ volatile("dsb sy" ::: "memory")

static uint32_t rd32(uintptr_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	dsb();
	return v;
}

static void wr32(uintptr_t base, uint32_t off, uint32_t val)
{
	dsb();
	*(volatile uint32_t *)(base + off) = val;
	dsb();
}

#else /* host: a register file plus the write trace host_test.c asserts on */

uint32_t starq_dsi_regs_ctrl[STARQ_DSI_CTRL_SIZE / 4];
uint32_t starq_dsi_regs_phy[STARQ_DSI_PHY_SIZE / 4];
struct starq_dsi_trace_ent starq_dsi_trace[STARQ_DSI_TRACE_MAX];
unsigned starq_dsi_trace_n;
unsigned starq_dsi_rd_n;
uint8_t starq_dsi_cmd_buf[STARQ_DCS_MAX_LEN + 8] __attribute__((aligned(8)));
int starq_dsi_sim_stall;
static int starq_dsi_use_fifo;

static uint32_t rd32(uintptr_t base, uint32_t off)
{
	starq_dsi_rd_n++;
	return base == STARQ_DSI_PHY_PA ? starq_dsi_regs_phy[off / 4]
					: starq_dsi_regs_ctrl[off / 4];
}

static void wr32(uintptr_t base, uint32_t off, uint32_t val)
{
	if (base == STARQ_DSI_PHY_PA) {
		starq_dsi_regs_phy[off / 4] = val;
		return;
	}
	starq_dsi_regs_ctrl[off / 4] = val;
	if (starq_dsi_trace_n < STARQ_DSI_TRACE_MAX) {
		starq_dsi_trace[starq_dsi_trace_n].off = off;
		starq_dsi_trace[starq_dsi_trace_n].val = val;
		starq_dsi_trace_n++;
	}
	/* Model the DMA engine: a software trigger raises DMA_DONE, unless the
	 * test asked this path to stall (to exercise the timeout and the
	 * memory->FIFO fallback). */
	if (off == R_TRIG_DMA && val == 1 &&
	    !(starq_dsi_sim_stall & (starq_dsi_use_fifo ? 2 : 1)))
		starq_dsi_regs_ctrl[R_INT_CTRL / 4] |= INT_CTRL_DMA_DONE;
	if (off == R_INT_CTRL && (val & INT_CTRL_DMA_DONE))
		starq_dsi_regs_ctrl[R_INT_CTRL / 4] &= ~INT_CTRL_DMA_DONE;
}

#endif

/* ---- logging, in the loader's console + ramoops style ---- */
static void reg_row(uintptr_t base, const char *n0, uint32_t o0,
		    const char *n1, uint32_t o1, const char *n2, uint32_t o2)
{
	starq_kv(n0, rd32(base, o0));
	starq_kv(n1, rd32(base, o1));
	starq_kv(n2, rd32(base, o2));
	starq_out("\n");
}

/* Exactly eight hex digits plus a trailing space: the log line is meant to be
 * pasted next to the vendor register dump, where "0x000001b3" and "0x1b3" are
 * not the same thing when you are looking for a bit. */
static void hex8(uint32_t v)
{
	static const char digits[] = "0123456789abcdef";
	char b[12] = "0x";

	for (int i = 0; i < 8; i++)
		b[2 + i] = digits[(v >> (28 - 4 * i)) & 0xf];
	b[10] = ' ';
	b[11] = 0;
	starq_out(b);
}

/* Log a polled value only when it changes: "every register poll result"
 * without filling the 256 KB ramoops zone with the same word 200k times. */
static void poll_log(uint32_t n, uint32_t val, uint32_t *last)
{
	if (val == *last)
		return;
	*last = val;
	starq_out("STARQ DSI: poll ");
	starq_kv("n", n);
	starq_out("int=");
	starq_hex(val);
	starq_out("\n");
}

int starq_dsi_init(uintptr_t ctrl_pa, uintptr_t phy_pa)
{
	uint32_t ctrl, intc, phy;

	dsi_ctrl = ctrl_pa;
	dsi_phy = phy_pa;
	/* Host: the packet buffer is an array, so the address the DMA is told
	 * about is its host address. */
#if defined(__aarch64__)
	dsi_cmd_pa = STARQ_DSI_CMD_BUF_PA;
#else
	dsi_cmd_pa = (uintptr_t)starq_dsi_cmd_buf;
#endif
	dsi_inited = 0;
	dsi_link_down = 1;		/* until DSI_CTRL proves otherwise */
	if (!ctrl_pa || !phy_pa) {
		starq_out("STARQ DSI: no controller address, TX disabled\n");
		return -1;
	}

	starq_out("STARQ DSI: ctrl=");
	starq_hex(ctrl_pa);
	starq_out(" phy=");
	starq_hex(phy_pa);
	starq_out(" buf=");
	starq_hex(dsi_cmd_pa);
	starq_out("\n");

	/*
	 * The mode probe. This is the single most important line this file
	 * prints: both S9 panels are dsi_cmd_mode in the vendor DTS, so a
	 * bootloader that leaves DSI0 in command mode means the framebuffer at
	 * FB_BASE is NOT what is on the panel and MDSS's write_memory_start
	 * (plus a full DSI bring-up) is mandatory rather than optional.
	 * Read only: nothing here is ever written.
	 */
	ctrl = rd32(dsi_ctrl, R_CTRL);
	starq_out("STARQ DSI: DSI_CTRL=");
	hex8(ctrl);
	starq_out("en=");
	starq_out((ctrl & CTRL_ENABLE) ? "1" : "0");
	starq_out(" video=");
	starq_out((ctrl & CTRL_VID_MODE_EN) ? "1" : "0");
	starq_out(" cmd=");
	starq_out((ctrl & CTRL_CMD_MODE_EN) ? "1" : "0");
	starq_out(" hwver=");
	hex8(rd32(dsi_ctrl, R_HW_VERSION));
	starq_kv("status", rd32(dsi_ctrl, R_STATUS));
	starq_kv("lanes", (ctrl >> 4) & 0xf);
	starq_out("\n");

	starq_out("STARQ DSI: ack_err=");
	hex8(rd32(dsi_ctrl, R_ACK_ERR_STATUS));
	starq_out("timeout=");
	hex8(rd32(dsi_ctrl, R_TIMEOUT_STATUS));
	starq_out("\n");

	/* Video timing: read-only, for the log and for the schedule line. */
	starq_out("STARQ DSI: ");
	reg_row(dsi_ctrl, "vid", R_VIDEO_MODE_CTRL, "active_v",
		R_VIDEO_MODE_ACTIVE_V, "total", R_VIDEO_MODE_TOTAL);

	starq_out("STARQ DSI: ");
	reg_row(dsi_ctrl, "dma_ctrl", R_CMD_DMA_CTRL, "cmd_off",
		R_DMA_CMD_OFFSET, "cmd_len", R_DMA_CMD_LENGTH);

	starq_out("STARQ DSI: ");
	reg_row(dsi_ctrl, "fifo", R_DMA_FIFO_CTRL, "trig", R_TRIG_CTRL,
		"int", R_INT_CTRL);

	starq_out("STARQ DSI: ");
	reg_row(dsi_ctrl, "mask0", R_ERR_INT_MASK0, "clk", R_CLK_CTRL,
		"ack_err", R_ACK_ERR_STATUS);

	intc = rd32(dsi_ctrl, R_INT_CTRL);

	/* PHY: snapshot only. Nothing here is ever written. */
	starq_out("STARQ DSI: phy ");
	starq_kv("sts", rd32(dsi_phy, P_PHY_STATUS));
	starq_kv("lane0", rd32(dsi_phy, P_LANE_STATUS0));
	starq_kv("lane1", rd32(dsi_phy, P_LANE_STATUS1));
	starq_out("\n");

	starq_out("STARQ DSI: phy ");
	starq_kv("clk0", rd32(dsi_phy, P_CLK_CFG0));
	starq_kv("clk1", rd32(dsi_phy, P_CLK_CFG1));
	starq_kv("vreg", rd32(dsi_phy, P_VREG_CTRL));
	starq_out("\n");

	starq_out("STARQ DSI: phy ");
	starq_kv("glbl", rd32(dsi_phy, P_GLBL_CTRL));
	starq_kv("timing0", rd32(dsi_phy, P_TIMING_CTRL_0));
	starq_out("\n");

	/* dsi_phy_hw_v3_0_power_on() polls PHY_STATUS bit 0 (REFGEN READY). */
	phy = rd32(dsi_phy, P_PHY_STATUS);
	starq_out(phy & 1u ? "STARQ DSI: refgen ready\n"
			   : "STARQ DSI: refgen NOT ready, link may be down\n");
	if (intc & INT_CTRL_ERROR)
		starq_out("STARQ DSI: error bit already latched\n");

	/*
	 * What mode the panel is in, in plain words, because this decides the
	 * whole display plan. DSI_CTRL.ENABLE clear means the link is down and
	 * every later send below becomes a no-op instead of a 200k-iteration
	 * poll against a dead controller.
	 */
	if (!(ctrl & CTRL_ENABLE)) {
		starq_out("STARQ DSI: LINK DOWN (DSI_CTRL.ENABLE=0), "
			  "no command will be sent\n");
		dsi_inited = 1;
		return 0;
	}
	dsi_link_down = 0;
	if (ctrl & CTRL_VID_MODE_EN) {
		starq_out("STARQ DSI: panel in VIDEO mode: the bootloader "
			  "framebuffer is being scanned out\n");
	} else if (ctrl & CTRL_CMD_MODE_EN) {
		starq_out("STARQ DSI: panel in COMMAND mode (vendor DT agrees, "
			  "dsi_cmd_mode): FB_BASE is NOT on screen unless MDSS "
			  "push/write_memory_start is programmed\n");
	} else {
		starq_out("STARQ DSI: neither video nor command mode set in "
			  "DSI_CTRL; display path unknown\n");
	}
	dsi_inited = 1;
	return 0;
}

void starq_dsi_set_sw_trigger(int on)
{
	dsi_sw_trigger = on;
}

/* ---- which panel is this ---- */

/* strstr, because there is no libc here. */
static int contains(const char *hay, const char *needle)
{
	for (; *hay; hay++) {
		const char *h = hay, *n = needle;

		while (*h && *n && *h == *n)
			h++, n++;
		if (!*n)
			return 1;
	}
	return 0;
}

/*
 * The bootloader names the panel in its own command line, which it puts in the
 * DTB's /chosen/bootargs; the vendor display driver parses the same string
 * (dsi_display.c:7061-7064) and matches the node name against panel->name
 * (ss_dsi_panel_common.c:4862-4876). Free of risk: it is a string ABL
 * already wrote, and reading it means no display register is touched.
 */
int starq_dsi_panel_from_cmdline(const char *bootargs, char *out,
				 unsigned outlen)
{
	static const char key[] = "msm_drm.dsi_display0=";
	const char *p, *v = 0;
	unsigned n = 0;

	if (out && outlen)
		out[0] = 0;
	if (!bootargs)
		return STARQ_PANEL_UNKNOWN;

	/* Token start only, and the key must start the token: the vendor
	 * property is a kernel module parameter, so "nosomethingmsm_drm..." and
	 * a mid-token lookalike must not match. */
	for (p = bootargs; *p; p++) {
		const char *k = key, *q;

		if (p != bootargs && p[-1] != ' ')
			continue;
		for (q = p; *k && *q == *k; q++, k++)
			;
		if (!*k) {
			v = q;		/* just past the '=' */
			break;
		}
	}
	if (!v)
		return STARQ_PANEL_UNKNOWN;

	/* The raw token, up to the space that ends it: the log line then reads
	 * "panel=PX01 name=ss_dsi_panel_S6E3HA8_AMB577PX01_WQHD:config0". */
	while (v[n] && v[n] != ' ' && n + 1 < outlen) {
		if (out)
			out[n] = v[n];
		n++;
	}
	if (out)
		out[n] = 0;

	if (out && contains(out, "AMB577PX01"))
		return STARQ_PANEL_PX01;
	if (out && contains(out, "AMB577MQ01"))
		return STARQ_PANEL_MQ01;
	return STARQ_PANEL_UNKNOWN;
}

/* ---- boot-arg parsing: "b5,8f,dc,0a" / "f0,5a,5a;b5,8f,dc,0a" ---- */

static int hexval(char c, unsigned *out)
{
	if (c >= '0' && c <= '9')
		*out = (unsigned)(c - '0');
	else if (c >= 'a' && c <= 'f')
		*out = (unsigned)(c - 'a' + 10);
	else if (c >= 'A' && c <= 'F')
		*out = (unsigned)(c - 'A' + 10);
	else
		return 0;
	return 1;
}

/*
 * Grammar: byte ("," byte)* (";" byte ("," byte)*)*
 * A byte is 1 or 2 hex digits. Whitespace is not accepted: the string arrives
 * from a boot-arg and any space would make it two tokens anyway. On any error
 * *out is zeroed, so a malformed string can never produce a partial send.
 */
int starq_dcs_parse(const char *s, struct starq_dcs_seq *out)
{
	unsigned n = 0, hi;

	out->n = 0;
	if (!s || !*s)
		return -1;

	while (*s) {
		if (n >= STARQ_DCS_MAX_CMD)
			goto bad;
		out->cmd[n].len = 0;

		for (;;) {
			unsigned v;

			if (!hexval(*s, &v))
				goto bad;
			s++;
			if (hexval(*s, &hi)) {	/* second digit of the byte */
				v = v << 4 | hi;
				s++;
			}
			if (v > 0xff || out->cmd[n].len >= STARQ_DCS_MAX_LEN)
				goto bad;
			out->cmd[n].data[out->cmd[n].len++] = (uint8_t)v;

			if (*s != ',')
				break;
			s++;
		}

		/* The only things that may follow a byte are ',' (consumed
		 * above), ';', or the end: "b5,8fdc" is not two bytes. */
		if (*s && *s != ';')
			goto bad;
		if (*s == ';') {
			s++;
			if (!*s)		/* trailing ';' */
				goto bad;
		}
		n++;
	}

	out->n = n;
	return n ? 0 : -1;
bad:
	out->n = 0;
	return -1;
}

/* ---- transmit ---- */

/* Build the memory packet. Returns the padded length, or 0 if unsupported. */
static unsigned build_packet(const uint8_t *p, unsigned len, uint8_t *buf)
{
	unsigned total, i;

	if (!len || len > STARQ_DCS_MAX_LEN)
		return 0;

	if (len <= 2) {
		/* Short packet: the parameters live in the word-count position. */
		buf[0] = p[0];
		buf[1] = len == 2 ? p[1] : 0;
		buf[2] = len == 2 ? DT_DCS_SHORT_WRITE_PARAM : DT_DCS_SHORT_WRITE;
		buf[3] = PKT_LAST;
		total = 4;
	} else {
		buf[0] = (uint8_t)(len & 0xff);
		buf[1] = (uint8_t)(len >> 8);
		buf[2] = DT_DCS_LONG_WRITE;
		buf[3] = PKT_LAST | PKT_LONG;
		for (i = 0; i < len; i++)
			buf[4 + i] = p[i];
		total = 4 + len;
	}

	/* Pad to a 4-byte multiple with 0xff, as both drivers do. */
	i = total;
	total = (total + 3) & ~3u;
	for (; i < total; i++)
		buf[i] = 0xff;

	return total <= DMA_MAX_BYTES ? total : 0;
}

/* Wait for the command DMA/engine to be idle. 0 = idle, -1 = timeout. */
static int wait_idle(void)
{
	uint32_t last = 0xffffffffu, st = 0;

	for (uint32_t n = 0; n < POLL_IDLE; n++) {
		st = rd32(dsi_ctrl, R_STATUS);
		if (!(st & (STATUS_CMD_DMA_BUSY | STATUS_CMD_ENGINE_BUSY)))
			return 0;	/* the normal case: nothing logged */
		poll_log(n, st, &last);
	}
	starq_out("STARQ DSI: cmd engine never idle ");
	starq_kv("status", st);
	starq_out("\n");
	return -1;
}

/*
 * Program the DMA schedule line so the hardware emits the packet inside the
 * vertical blanking window instead of racing the video engine
 * (dsi_ctrl_hw_22_schedule_dma_cmd). Reads video timing only.
 */
static void schedule_in_vblank(int on)
{
	uint32_t line;

	if (!on) {
		wr32(dsi_ctrl, R_DMA_SCHED, 0);
		return;
	}
	/* active_v = (end << 16) | start; end is the first blanking line, i.e.
	 * v_sync_width + v_back_porch + v_active as dsi_message_tx() computes. */
	line = rd32(dsi_ctrl, R_VIDEO_MODE_ACTIVE_V) >> 16;
	if (!line) {
		starq_out("STARQ DSI: no vblank line in ACTIVE_V, no schedule\n");
		return;
	}
	starq_out("STARQ DSI: sched ");
	starq_kv("line", line);
	starq_out("\n");
	wr32(dsi_ctrl, R_DMA_SCHED, DMA_SCHED_EN | (line & 0xffffu));
}

/* Common tail of both paths: trigger, then poll DMA_DONE. */
static int trigger_and_wait(void)
{
	uint32_t last = 0xffffffffu, v;

	/* Clear a stale completion first (write 1 to clear, keeping the mask
	 * bits, as dsi_ctrl_hw_cmn_clear_interrupt_status does). */
	v = rd32(dsi_ctrl, R_INT_CTRL);
	wr32(dsi_ctrl, R_INT_CTRL, v | INT_CTRL_DMA_DONE);

	wr32(dsi_ctrl, R_TRIG_DMA, 1);

	for (uint32_t n = 0; n < POLL_DONE; n++) {
		v = rd32(dsi_ctrl, R_INT_CTRL);
		poll_log(n, v, &last);
		if (v & INT_CTRL_DMA_DONE) {
			wr32(dsi_ctrl, R_INT_CTRL, v | INT_CTRL_DMA_DONE);
			return 0;
		}
	}
	starq_out("STARQ DSI: DMA done timeout ");
	starq_kv("int", v);
	starq_kv("status", rd32(dsi_ctrl, R_STATUS));
	starq_out("\n");
	return -1;
}

/* Path 1: packet in memory, the controller fetches it (kickoff_command). */
static int dma_from_memory(uintptr_t pa, unsigned len)
{
	uint32_t reg;

	reg = rd32(dsi_ctrl, R_CMD_DMA_CTRL);
	/* Embedded mode: header comes from memory, not from registers. */
	reg |= DMA_CTRL_EMBEDDED;
	reg &= ~(DMA_CTRL_PKT_TYPE | DMA_CTRL_WC_SEL);
	/* Single controller, no broadcast, HS (not low-power) transmission. */
	reg &= ~(DMA_CTRL_BROADCAST | DMA_CTRL_IS_MASTER | DMA_CTRL_LPM);
	wr32(dsi_ctrl, R_CMD_DMA_CTRL, reg);
	starq_kv("dma_ctrl", reg);

	/* Disable the FIFO watermarks: one packet, nothing to wait for. */
	reg = rd32(dsi_ctrl, R_DMA_FIFO_CTRL);
	wr32(dsi_ctrl, R_DMA_FIFO_CTRL, reg | (1u << 20) | (1u << 16));

	wr32(dsi_ctrl, R_DMA_CMD_OFFSET, (uint32_t)pa);
	wr32(dsi_ctrl, R_DMA_CMD_LENGTH, len & 0xffffffu);
	starq_out("\n");
	return trigger_and_wait();
}

/* Path 2: packet pushed into the command TPG FIFO (kickoff_fifo_command). */
static int dma_from_fifo(const uint8_t *buf, unsigned len)
{
	const uint32_t *w = (const uint32_t *)buf;
	unsigned words = len / 4;

	if (len > FIFO_MAX_BYTES)
		return -1;

	wr32(dsi_ctrl, R_TPG_CTRL, TPG_CMD_DMA_FIFO);
	for (unsigned i = 0; i < words; i++)
		wr32(dsi_ctrl, R_TPG_CMD_DMA_INIT, w[i]);
	/* "Hardware requires an extra DWORD (set to zero) if the length of
	 * command buffer is odd DWORDS." */
	if (words & 1u)
		wr32(dsi_ctrl, R_TPG_CMD_DMA_INIT, 0);

	wr32(dsi_ctrl, R_CMD_DMA_CTRL,
	     (rd32(dsi_ctrl, R_CMD_DMA_CTRL) &
	      ~(DMA_CTRL_BROADCAST | DMA_CTRL_IS_MASTER | DMA_CTRL_LPM)) |
	     DMA_CTRL_EMBEDDED);
	wr32(dsi_ctrl, R_DMA_CMD_LENGTH, len);
	return trigger_and_wait();
}

/* reset_cmd_fifo(): leave the TPG the way we found it. */
static void reset_cmd_fifo(void)
{
	wr32(dsi_ctrl, R_TPG_CTRL, 0);
	wr32(dsi_ctrl, R_TPG_DMA_FIFO_RESET, 1);
	wr32(dsi_ctrl, R_TPG_DMA_FIFO_RESET, 0);
}

static void log_bytes(const uint8_t *p, unsigned len)
{
	static const char digits[] = "0123456789abcdef";

	starq_out("STARQ DSI: dcs");
	for (unsigned i = 0; i < len; i++) {
		char b[3] = { digits[p[i] >> 4], digits[p[i] & 0xf], 0 };
		starq_out(" ");
		starq_out(b);
	}
	starq_out("\n");
}

int starq_dsi_dcs_write(const uint8_t *payload, unsigned len)
{
	uint8_t *buf = cmd_buf();
	unsigned total;
	int err;

	if (!dsi_inited) {
		starq_out("STARQ DSI: not initialised, command dropped\n");
		return -1;
	}
	if (dsi_link_down) {
		starq_out("STARQ DSI: link down, command dropped\n");
		return -1;
	}
	if (!payload || !len || len > STARQ_DCS_MAX_LEN) {
		starq_out("STARQ DSI: bad DCS length ");
		starq_kv("len", len);
		starq_out("\n");
		return -1;
	}
	total = build_packet(payload, len, buf);
	if (!total) {
		starq_out("STARQ DSI: packet does not fit DMA_MAX_BYTES\n");
		return -1;
	}
	log_bytes(payload, len);
	starq_kv("pkt", total);
	starq_out("\n");

	if (wait_idle())
		return -1;

	/* In video mode the packet has to go out during blanking; in command
	 * mode the trigger is the frame update and there is nothing to dodge. */
	if (!dsi_sw_trigger &&
	    (rd32(dsi_ctrl, R_CTRL) & CTRL_VID_MODE_EN))
		schedule_in_vblank(1);

#if !defined(__aarch64__)
	starq_dsi_use_fifo = 0;
#endif
	err = dma_from_memory(dsi_cmd_pa, total);
	if (err) {
		starq_out("STARQ DSI: memory path failed, FIFO fallback\n");
		reset_cmd_fifo();
#if !defined(__aarch64__)
		starq_dsi_use_fifo = 1;
#endif
		err = dma_from_fifo(buf, total);
		reset_cmd_fifo();
	}
	if (!dsi_sw_trigger)
		schedule_in_vblank(0);

	if (err) {
		starq_out("STARQ DSI: command FAILED\n");
		return -1;
	}
	starq_out("STARQ DSI: command sent\n");
	return 0;
}

/* Send a parsed sequence in order, stopping at the first failure. */
static int send_seq(const struct starq_dcs_seq *seq)
{
	int sent = 0;

	if (!dsi_inited) {
		starq_out("STARQ DSI: not initialised, sequence dropped\n");
		return -1;
	}
	if (dsi_link_down) {
		starq_out("STARQ DSI: link down, sequence dropped\n");
		return -1;
	}
	starq_out("STARQ DSI: sequence ");
	starq_kv("cmds", seq->n);
	starq_out("\n");
	for (unsigned i = 0; i < seq->n; i++) {
		if (starq_dsi_dcs_write(seq->cmd[i].data, seq->cmd[i].len)) {
			starq_out("STARQ DSI: stopped at command ");
			starq_kv("n", i);
			starq_out("\n");
			return -1;
		}
		sent++;
	}
	return sent;
}

int starq_dsi_run(const char *spec)
{
	struct starq_dcs_seq seq;

	if (!dsi_inited) {
		starq_out("STARQ DSI: not initialised, sequence dropped\n");
		return -1;
	}
	if (!spec || !*spec)		/* nothing to send */
		return 0;
	if (starq_dcs_parse(spec, &seq)) {
		starq_out("STARQ DSI: bad hex list \"");
		for (int i = 0; spec[i] && i < 64; i++) {
			char c[2] = { spec[i], 0 };
			starq_out(c);
		}
		starq_out("\": nothing sent\n");
		return -1;
	}
	return send_seq(&seq);
}

/* The default sequence: level-1 key on, one 4-byte 0xB5, level-1 key off. */
int starq_dsi_default(int panel, unsigned mps)
{
	struct starq_dcs_seq seq;
	unsigned elvss = panel == STARQ_PANEL_MQ01 ? STARQ_ELVSS_MQ01
						  : STARQ_ELVSS_UNKNOWN;
	unsigned n = 0;

	seq.cmd[n].data[0] = 0xf0;
	seq.cmd[n].data[1] = 0x5a;
	seq.cmd[n].data[2] = 0x5a;
	seq.cmd[n].len = 3;
	n++;
	seq.cmd[n].data[0] = 0xb5;
	seq.cmd[n].data[1] = STARQ_ELVSS_TSET;
	seq.cmd[n].data[2] = (uint8_t)mps;
	seq.cmd[n].data[3] = (uint8_t)elvss;
	seq.cmd[n].len = 4;
	n++;
	seq.cmd[n].data[0] = 0xf0;
	seq.cmd[n].data[1] = 0xa5;
	seq.cmd[n].data[2] = 0xa5;
	seq.cmd[n].len = 3;
	n++;
	seq.n = n;
	return send_seq(&seq);
}

/* "off": the whole override disabled - nothing parsed, nothing sent. */
int starq_elvss_off(const char *spec)
{
	return spec && spec[0] == 'o' && spec[1] == 'f' && spec[2] == 'f' &&
	       spec[3] == 0;
}

/*
 * The policy, in one place so the loader and the kernel side can be read
 * against each other: an explicit boot-arg wins, "off" disables, and an absent
 * boot-arg means the panel-aware default runs (the override is on by default
 * because the green tint is the reason this code exists).
 */
int starq_dsi_elvss(const char *spec, int panel, unsigned mps)
{
	if (starq_elvss_off(spec)) {
		starq_out("STARQ DSI: starqelvss=off, override disabled\n");
		return 0;
	}
	if (spec && *spec) {
		/* A starqelvss= boot-arg, or a non-empty STARQ_ELVSS_DEFAULT
		 * baked in at build time: both mean "use these bytes as given". */
		starq_out("STARQ DSI: explicit sequence from the boot-arg/compiled"
			  " default\n");
		return starq_dsi_run(spec);
	}

	starq_out("STARQ DSI: default policy, panel=");
	starq_out(panel == STARQ_PANEL_PX01 ? "PX01" :
		  panel == STARQ_PANEL_MQ01 ? "MQ01" : "UNKNOWN");
	starq_kv("mps", mps);
	starq_kv("elvss", panel == STARQ_PANEL_MQ01 ? STARQ_ELVSS_MQ01
						    : STARQ_ELVSS_UNKNOWN);
	starq_out("\n");
	if (panel == STARQ_PANEL_UNKNOWN)
		starq_out("STARQ DSI: no AMB577 panel name in the bootloader "
			  "cmdline, assuming PX01: it is the higher-volume "
			  "starqlte panel and 0x16 is the more compensated "
			  "of the two, so a wrong guess errs towards less "
			  "green\n");
	return starq_dsi_default(panel, mps);
}
