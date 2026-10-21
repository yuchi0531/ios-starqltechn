#pragma once
#include <stdint.h>

/*
 * SDM845 DSI0 command-path TX (Galaxy S9, "starqltechn").
 *
 * S-Boot leaves MDSS + DSI0 + the S6E3HA8 panel powered, PLL-locked and
 * streaming in video mode. Nothing here initialises the link: it only sends
 * MIPI DCS packets through the DSI command DMA, which is an independent engine
 * from the video engine, and takes a read-only snapshot of everything it must
 * not disturb.
 *
 * Addresses from the vendor DT (arch/arm64/boot/dts/samsung/
 * sdm845-sec-starqlte-r14.dts, confirmed in the extracted
 * /tmp/opencode/starq-vendor.dts):
 *
 *   qcom,mdss_dsi_ctrl0@ae94000  reg = <0xae94000 0x400 0xaf08000 0x4>
 *   qcom,mdss_dsi_phy0@ae94400   reg = <0xae94400 0x7c0>
 *
 * and mapped to these labels (sdm845-sec-starqlte-r14.dts:25739-25741):
 *   mdss_dsi0 = "/soc/qcom,mdss_dsi_ctrl0@ae94000"
 *   mdss_dsi_phy0 = "/soc/qcom,mdss_dsi_phy0@ae94400"
 */
#ifndef STARQ_DSI_CTRL_PA
#define STARQ_DSI_CTRL_PA	0xae94000UL	/* DSI0 controller, 0x400 */
#endif
#ifndef STARQ_DSI_CTRL_SIZE
#define STARQ_DSI_CTRL_SIZE	0x400UL
#endif
#ifndef STARQ_DSI_PHY_PA
#define STARQ_DSI_PHY_PA	0xae94400UL	/* DSI0 PHY (v3.0), 0x7c0 */
#endif
#ifndef STARQ_DSI_PHY_SIZE
#define STARQ_DSI_PHY_SIZE	0x7c0UL
#endif

/*
 * Command-DMA packet buffer. The MMU is off in the loader, so this is the
 * physical address the DSI DMA engine reads; we must not program the MDSS
 * SMMU, so it has to be memory the display pipeline already has mapped --
 * i.e. inside the bootloader framebuffer reservation ("framebuffer_mem",
 * 0x9d400000 + 0x02400000, the only region S-Boot mapped for MDSS). The
 * visible panel area is the first 1440*2960*4 = 0x1042800 bytes, so the last
 * page is outside anything the test pattern or XNU's console touches.
 */
#ifndef STARQ_DSI_CMD_BUF_PA
#define STARQ_DSI_CMD_BUF_PA	0x9f7ff000UL	/* 0x9d400000 + 0x2400000 - 0x1000 */
#endif

/* One parsed MIPI DCS message. */
#define STARQ_DCS_MAX_LEN	64
#define STARQ_DCS_MAX_CMD	8
struct starq_dcs_cmd {
	uint8_t data[STARQ_DCS_MAX_LEN];
	unsigned len;
};
struct starq_dcs_seq {
	struct starq_dcs_cmd cmd[STARQ_DCS_MAX_CMD];
	unsigned n;
};

/*
 * "b5,8f,dc,0a"             one 4-byte DCS write
 * "f0,5a,5a;b5,8f,dc,0a"    two writes, in order
 * Bytes are 1-2 hex digits. Anything else is rejected and out->n is left 0,
 * so a malformed string never turns into a partial send.
 */
int starq_dcs_parse(const char *s, struct starq_dcs_seq *out);

/* Map-and-snapshot (read only) the controller and PHY registers. */
int starq_dsi_init(uintptr_t ctrl_pa, uintptr_t phy_pa);

/*
 * starqdsisw=1: do not program DSI_DMA_SCHEDULE_CTRL, just software-trigger.
 * mainline's behaviour (send whenever the DMA engine is idle) instead of the
 * vendor 2.2 driver's scan-line scheduling. Default 0 (schedule in blanking).
 */
void starq_dsi_set_sw_trigger(int on);

/* Send one DCS message: short write for len 1-2, long write otherwise. */
int starq_dsi_dcs_write(const uint8_t *payload, unsigned len);

/* Parse "spec" and send it. Logs as STARQ DSI: lines. 0 = at least one sent. */
int starq_dsi_run(const char *spec);

/* Register writes the driver performed, most recent transfer only. The host
 * test (host_test.c) verifies the register sequence from this; on aarch64 it is
 * not compiled, the real log lines carry the same information. */
#define STARQ_DSI_TRACE_MAX	96
struct starq_dsi_trace_ent {
	uint32_t off, val;
};
#if !defined(__aarch64__)
extern struct starq_dsi_trace_ent starq_dsi_trace[STARQ_DSI_TRACE_MAX];
extern unsigned starq_dsi_trace_n;
/* MMIO stand-ins: dsi.c maps the controller and the PHY onto these instead of
 * the real addresses, so the host test can inspect every write. */
extern uint32_t starq_dsi_regs_ctrl[STARQ_DSI_CTRL_SIZE / 4];
extern uint32_t starq_dsi_regs_phy[STARQ_DSI_PHY_SIZE / 4];
/* The DMA-visible packet buffer (STARQ_DSI_CMD_BUF_PA on the phone). */
extern uint8_t starq_dsi_cmd_buf[STARQ_DCS_MAX_LEN + 8];
/* bit0: the memory path never completes, bit1: the FIFO path never completes.
 * Lets host_test.c exercise the timeout and the memory->FIFO fallback. */
extern int starq_dsi_sim_stall;
#endif
