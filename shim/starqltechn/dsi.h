#pragma once
#include <stdint.h>

/*
 * SDM845 DSI0 command-path TX (Galaxy S9, "starqltechn").
 *
 * ABL leaves MDSS, DSI0 and the S6E3HA8 panel powered and PLL-locked. What
 * mode the panel is in is NOT assumed here: the vendor DTS declares both S9
 * panels dsi_cmd_mode (dtsi_px01.dtsi:70, dtsi_mq01.dtsi:66), which is why
 * starq_dsi_init() logs DSI_CTRL's enable/video/command bits and this code
 * branches on the register rather than on the DT. Nothing here initialises the
 * link: it only sends MIPI DCS packets through the DSI command DMA, which is an
 * independent engine from the video engine, and takes a read-only snapshot of
 * everything it must not disturb.
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
 * 0x9d400000 + 0x02400000, the only region ABL mapped for MDSS). The
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

/*
 * Which S9 panel this unit has. The two panels take a different 4th byte in
 * the 0xB5 ELVSS write (AMB577PX01: 0x16, AMB577MQ01: 0x0A), so it cannot be a
 * constant.
 */
#define STARQ_PANEL_UNKNOWN	0
#define STARQ_PANEL_PX01	1	/* AMB577PX01, dtsi_px01.dtsi */
#define STARQ_PANEL_MQ01	2	/* AMB577MQ01, dtsi_mq01.dtsi */

/*
 * Which panel the bootloader configured, from the DTB's /chosen/bootargs:
 * ABL puts its merged Linux command line there and the vendor display driver
 * names the panel in it, e.g.
 *   msm_drm.dsi_display0=ss_dsi_panel_S6E3HA8_AMB577PX01_WQHD:config0
 * (dsi_display.c:7061-7064, matched against panel->name in
 * ss_dsi_panel_common.c:4862-4876). `out` receives that raw token for the log,
 * or an empty string when it is absent. Returns one of STARQ_PANEL_*.
 */
int starq_dsi_panel_from_cmdline(const char *bootargs, char *out,
				 unsigned outlen);

/* Map-and-snapshot (read only) the controller and PHY registers, and log the
 * link mode. Clears the link-down flag when DSI_CTRL.ENABLE is set; when it is
 * clear every later send is a no-op. */
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

/*
 * The ELVSS default policy. Mechanism, all from the vendor driver and its DTS:
 *
 *   - 0xB5 is a 24-byte page on PX01 and a 4-byte command on MQ01, but only
 *     bytes 1..3 are what the driver ever computes; byte 23 on PX01 is a
 *     per-panel OTP black-level trim that must be preserved, not synthesised.
 *     So a 4-byte write is the universal move: it leaves PX01's bytes 4..23
 *     (including that trim) untouched.
 *   - 0xB5 sits in the level-1 protected bank (PX01's own gray-spot test
 *     wraps it in F0 5A 5A / F0 A5 A5, dtsi_px01.dtsi:1905-1919, and
 *     ss_elvss() sets *level_key = LEVEL1_KEY), so the write is bracketed by
 *     the key.
 *   - byte 1 (TSET) 0x8F = BIT(7) | 15 is the panel's -15 C compensation row,
 *     which is what the Android Magisk module forces to kill the green tint.
 *   - byte 2 (MPS) is 0xDC while the panel is above 39 cd.
 *   - byte 3 (ELVSS offset) is 0x16 on PX01 and 0x0A on MQ01 (both DT's
 *     index-0 entry).
 *
 * Sends "f0 5a 5a" / "b5 8f <mps> <elvss>" / "f0 a5 a5". Returns the number of
 * commands sent, or -1.
 */
#define STARQ_ELVSS_TSET	0x8f	/* BIT(7) | 15 = -15 C */
#define STARQ_ELVSS_MPS		0xdc	/* candela_level > 39 */
#define STARQ_ELVSS_PX01	0x16
#define STARQ_ELVSS_MQ01	0x0a
/* Unknown panel: the PX01 value, because PX01 is the higher-volume starqlte
 * panel and 0x16 is the larger (more compensated) of the two, so a wrong guess
 * errs towards less green rather than more. */
#define STARQ_ELVSS_UNKNOWN	STARQ_ELVSS_PX01

int starq_dsi_default(int panel, unsigned mps);

/* 1 when spec asks for the whole override to be off ("off"). */
int starq_elvss_off(const char *spec);

/*
 * Apply the policy: spec == NULL/"" takes the default above, "off" does
 * nothing, anything else is the starqelvss=<hex list> override (all or
 * nothing). Returns the number of commands sent, or -1.
 */
int starq_dsi_elvss(const char *spec, int panel, unsigned mps);

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
/* Register reads since the test last cleared it. Loader.c must leave this at 0
 * when starqelvss=off or starqdsi=0: the point of those switches is that not
 * one DSI register is touched, not just that nothing is sent. */
extern unsigned starq_dsi_rd_n;
/* The DMA-visible packet buffer (STARQ_DSI_CMD_BUF_PA on the phone). */
extern uint8_t starq_dsi_cmd_buf[STARQ_DCS_MAX_LEN + 8];
/* bit0: the memory path never completes, bit1: the FIFO path never completes.
 * Lets host_test.c exercise the timeout and the memory->FIFO fallback. */
extern int starq_dsi_sim_stall;
#endif
