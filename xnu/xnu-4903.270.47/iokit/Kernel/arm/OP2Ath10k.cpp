// SPDX-License-Identifier: ISC
/*
 * OP2Ath10k: Wi-Fi step 2b for the OnePlus 2's QCA6174 (docs/WIFI-BT-PLAN.md, first half of P1).
 * Polled, no interrupts, no firmware:
 *   1. wait for the boot ROM's FW_INDICATOR "initialized" (qcacld-2.0 hif_pci_configure: 10 ms polls, 10 s)
 *   2. one 16 KB DMA block below 4 GB (cached; cleaned/invalidated around every device access, as OP2GPU
 *      does: the chip's DMA does not snoop the CPU caches), ASPM off on the chip, bus mastering on
 *   3. host side of copy engines CE0 (host->target: BMI requests), CE1 (target->host: BMI responses) and
 *      CE7 (the diagnostic window into target memory)
 *   4. first DMA: read hi_interconnect_state through CE7
 *   5. ath10k_pci_init_config(): target CE config, service map, L1 off, early alloc (9 IRAM banks on hw3.x),
 *      HI_OPTION_EARLY_CFG_DONE; then ath10k_pci_wake_target_cpu()
 *   6. BMI_GET_TARGET_INFO over CE0/CE1 (hw3.2 answers version 0x05030000)
 * Afterwards bus mastering is switched off again and the DMA block is never freed, so nothing the chip might
 * still write can land in memory that belongs to someone else.
 *
 * Ported from Linux drivers/net/wireless/ath/ath10k (ce.c, pci.c, bmi.c, hw.c, targaddrs.h; v7.4-rc4):
 *
 * Copyright (c) 2005-2011 Atheros Communications Inc.
 * Copyright (c) 2011-2017 Qualcomm Atheros, Inc.
 * Copyright (c) 2018 The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_MSM8994)

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include "op2_ath10k.h"

#define A_LOG(fmt, ...) IOLog("OP2 WIFI: 2b: " fmt "\n", __VA_ARGS__)   /* always >= 1 argument */

#if defined(__arm64__) || defined(__aarch64__)
#define A_DSB()         __asm__ volatile("dsb sy" ::: "memory")
#define A_DC_CIVAC(a)   __asm__ volatile("dc civac, %0" : : "r"(a) : "memory")
#else   /* host-side simulation test only */
#define A_DSB()         __asm__ volatile("" ::: "memory")
#define A_DC_CIVAC(a)   ((void)(a))
#endif

/* ---- BAR0 offsets (ath10k hw.c qca6174_regs and qcax_ce_regs, hw.h) ---- */
#define Q_RTC_STATE             0x00080000u   /* PCIe local; bits 2:0 == 3: awake */
#define Q_SOC_WAKE              0x00080004u
#define Q_CORE_CTRL             0x0003a000u   /* SOC_CORE + 0x00 */
#define Q_CORE_CTRL_CPU_INTR    0x00002000u
#define Q_FW_INDICATOR          0x0003a028u
#define Q_FW_IND_EVENT_PENDING  0x1u
#define Q_FW_IND_INITIALIZED    0x2u
#define Q_CE(n)                 (0x00034400u + (uint32_t)(n) * 0x400u)   /* CE0 0x34400 .. CE7 0x36000 */

#define CE_SR_BA            0x00
#define CE_SR_SIZE          0x04
#define CE_DR_BA            0x08
#define CE_DR_SIZE          0x0c
#define CE_CTRL1            0x10   /* 15:0 dmax, 16 src ring byte swap, 17 dst ring byte swap */
#define CE_HOST_IE          0x2c   /* 0 copy complete, 4:1 watermarks */
#define CE_HOST_IS          0x30   /* write 1 to clear */
#define CE_MISC_IE          0x34   /* 10:5 errors */
#define CE_MISC_IS          0x38
#define CE_SR_WR_INDEX      0x3c
#define CE_DST_WR_INDEX     0x40
#define CE_CURRENT_SRRI     0x44
#define CE_CURRENT_DRRI     0x48
#define CE_SRC_WATERMARK    0x4c   /* 31:16 low, 15:0 high */
#define CE_DST_WATERMARK    0x50

/* ---- target memory (targaddrs.h, pci.h) ---- */
#define HI_BASE                 0x00400800u   /* QCA988X_HOST_INTEREST_ADDRESS: used for every PCI chip */
#define HI_OPTION_FLAG2         (HI_BASE + 0xccu)
#define HI_INTERCONNECT_STATE   (HI_BASE + 0xf8u)
#define HI_EARLY_ALLOC          (HI_BASE + 0x100u)
#define HI_OPTION_EARLY_CFG_DONE 0x10u
#define HI_EARLY_ALLOC_MAGIC    0x6d8au       /* bits 31:16 */
#define HW3_IRAM_BANKS          9u            /* ath10k_pci_get_num_banks(): QCA6174 hw3.0 / 3.1 / 3.2 */
#define PCIE_STATE_PIPE_CFG     0x00u         /* struct pcie_state */
#define PCIE_STATE_SVC_MAP      0x04u
#define PCIE_STATE_CONFIG_FLAGS 0x20u
#define PCIE_CONFIG_FLAG_ENABLE_L1 0x1u

/* ---- BMI (bmi.h, hw.h) ---- */
#define BMI_GET_TARGET_INFO     8u
#define QCA6174_HW_3_2_VERSION  0x05030000u
#define BMI_SEND_FLAGS          0xFFFCu       /* ath10k_ce_send(..., transfer_id -1): META_DATA bits 15:2 set */

/* ---- the DMA block; every area on its own 64-byte cache lines ---- */
#define DMA_SIZE        0x4000u
#define OFF_CE0_SRC     0x0000u   /* 16 descriptors x 8 B */
#define OFF_CE1_DST     0x0100u   /* 16 x 8 B */
#define OFF_CE7_SRC     0x0200u   /*  2 x 8 B */
#define OFF_CE7_DST     0x0240u   /*  2 x 8 B */
#define OFF_BMI_REQ     0x1000u   /* 256 B */
#define OFF_BMI_RESP    0x1800u   /* 2 KB */
#define OFF_DIAG        0x2000u   /* 2 KB = DIAG_TRANSFER_LIMIT */
#define DIAG_MAX        2048u
#define BMI_REQ_MAX     256u
#define BMI_RESP_MAX    2048u

/* pci_target_ce_config_wlan[0..6] (NUM_TARGET_CE_CONFIG_WLAN = 7 on QCA6174), struct ce_pipe_config:
 * pipenum, pipedir (1 target->host, 2 host->target, 3 both), nentries, nbytes_max, flags, reserved */
static const uint32_t kTargetCeConfig[7][6] = {
	{ 0, 2, 32, 256, 0, 0 },     /* HTC control and raw streams */
	{ 1, 1, 32, 2048, 0, 0 },    /* HTT + HTC control */
	{ 2, 1, 64, 2048, 0, 0 },    /* WMI events */
	{ 3, 2, 32, 2048, 0, 0 },    /* WMI commands */
	{ 4, 2, 256, 256, 0, 0 },    /* HTT tx */
	{ 5, 1, 32, 512, 0, 0 },     /* HTT rx (HIF->HTT) */
	{ 6, 3, 32, 4096, 0, 0 },    /* target autonomous hif_memcpy */
};

/* pci_target_service_to_ce_map_wlan: HTC service id (group << 8 | index), pipedir, pipenum; zero entry ends it */
static const uint32_t kServiceMap[17][3] = {
	{ 0x0104, 2, 3 }, { 0x0104, 1, 2 },   /* WMI_DATA_VO */
	{ 0x0102, 2, 3 }, { 0x0102, 1, 2 },   /* WMI_DATA_BK */
	{ 0x0101, 2, 3 }, { 0x0101, 1, 2 },   /* WMI_DATA_BE */
	{ 0x0103, 2, 3 }, { 0x0103, 1, 2 },   /* WMI_DATA_VI */
	{ 0x0100, 2, 3 }, { 0x0100, 1, 2 },   /* WMI_CONTROL */
	{ 0x0001, 2, 0 }, { 0x0001, 1, 1 },   /* RSVD_CTRL */
	{ 0xFE00, 2, 0 }, { 0xFE00, 1, 1 },   /* TEST_RAW_STREAMS */
	{ 0x0300, 2, 4 }, { 0x0300, 1, 5 },   /* HTT_DATA_MSG */
	{ 0, 0, 0 },
};

struct ce_ring {
	uint32_t *desc;         /* nentries x { addr, nbytes | flags << 16 }, CPU view */
	uint32_t phys;          /* bus address of desc[0] */
	uint32_t nentries;      /* power of 2 */
	uint32_t sw_index;      /* next descriptor to complete */
	uint32_t write_index;   /* next descriptor to fill */
	uint32_t hw_index;      /* src ring: last CURRENT_SRRI read */
};

struct ce_pipe {
	uint32_t base;          /* BAR0 offset of this copy engine's registers */
	struct ce_ring src;
	struct ce_ring dst;
};

static volatile uint32_t *gB, *gConf, *gDbi;
static uint64_t gPowerOnAbs;
static uint8_t *gDma;       /* never freed */
static uint32_t gDmaPhys;
static struct ce_pipe gCe0, gCe1, gCe7;

static inline uint32_t
rd32(volatile uint32_t *b, uint32_t off)
{
	return b[off / 4];
}

static inline void
wr32(volatile uint32_t *b, uint32_t off, uint32_t v)
{
	b[off / 4] = v;
	A_DSB();
}

static inline uint32_t
brd(uint32_t off)
{
	return rd32(gB, off);
}

static inline void
bwr(uint32_t off, uint32_t v)
{
	wr32(gB, off, v);
}

static inline uint32_t *
dma_words(uint32_t off)
{
	return (uint32_t *)(void *)(gDma + off);
}

/* Clean + invalidate to the point of coherency (as OP2GPU's flushDcache): before the chip reads what the CPU
 * wrote, and before the CPU reads what the chip wrote. */
static inline void
dma_sync(const void *va, uint32_t size)
{
	uintptr_t a = (uintptr_t)va & ~(uintptr_t)63u;
	uintptr_t end = ((uintptr_t)va + size + 63u) & ~(uintptr_t)63u;
	A_DSB();
	for (; a < end; a += 64u) {
		A_DC_CIVAC(a);
	}
	A_DSB();
}

static uint32_t
ms_since_power_on(void)
{
	uint64_t now = 0, ns = 0;
	clock_get_uptime(&now);
	absolutetime_to_nanoseconds(now - gPowerOnAbs, &ns);
	return (uint32_t)(ns / 1000000u);
}

/* Keep the SoC awake for register access (ath10k_pci_wake_wait: 30 ms limit). */
static bool
q_wake(void)
{
	bwr(Q_SOC_WAKE, 1u);
	for (int us = 0; us < 30000; us += 50) {
		if ((brd(Q_RTC_STATE) & 7u) == 3u) {
			return true;
		}
		IODelay(50);
	}
	return false;
}

/* ath10k_pci_qca6174_targ_cpu_to_ce_addr: target CPU address -> the copy engines' address space */
static uint32_t
targ_to_ce(uint32_t a)
{
	return ((brd(Q_CORE_CTRL) & 0x7ffu) << 21) | ((a >= 0x100000u) ? 0x100000u : 0u) | (a & 0xfffffu);
}

static uint32_t
chip_pcie_cap(void)
{
	uint32_t p = rd32(gConf, 0x34) & 0xFCu;
	for (int i = 0; i < 48 && p >= 0x40u; i++) {
		uint32_t h = rd32(gConf, p);
		if ((h & 0xFFu) == 0x10u) {
			return p;
		}
		p = (h >> 8) & 0xFCu;
	}
	return 0;
}

/* ---------------------------------------------------------------------------------------------------- */
/* copy engines (ce.c)                                                                                  */

/* Polled: copy-complete, watermark and error interrupts off (ath10k_ce_disable_interrupt). */
static void
ce_quiet(const struct ce_pipe *p)
{
	bwr(p->base + CE_HOST_IE, brd(p->base + CE_HOST_IE) & ~0x1Fu);
	bwr(p->base + CE_MISC_IE, brd(p->base + CE_MISC_IE) & ~0x7E0u);
	bwr(p->base + CE_HOST_IS, 0x1Fu);
}

/* ath10k_ce_init_src_ring: software indexes start where the hardware's are */
static void
ce_init_src(struct ce_pipe *p, uint32_t off, uint32_t nentries, uint32_t dmax)
{
	struct ce_ring *r = &p->src;
	r->desc = dma_words(off);
	r->phys = gDmaPhys + off;
	r->nentries = nentries;
	bzero(r->desc, nentries * 8u);
	dma_sync(r->desc, nentries * 8u);
	r->sw_index = brd(p->base + CE_CURRENT_SRRI) & (nentries - 1u);
	r->hw_index = r->sw_index;
	r->write_index = brd(p->base + CE_SR_WR_INDEX) & (nentries - 1u);
	bwr(p->base + CE_SR_BA, r->phys);
	bwr(p->base + CE_SR_SIZE, nentries);
	bwr(p->base + CE_CTRL1, (brd(p->base + CE_CTRL1) & ~0x1FFFFu) | (dmax & 0xFFFFu));   /* dmax, no byte swap */
	bwr(p->base + CE_SRC_WATERMARK, nentries & 0xFFFFu);                                /* low 0, high nentries */
}

/* ath10k_ce_init_dest_ring */
static void
ce_init_dst(struct ce_pipe *p, uint32_t off, uint32_t nentries)
{
	struct ce_ring *r = &p->dst;
	r->desc = dma_words(off);
	r->phys = gDmaPhys + off;
	r->nentries = nentries;
	bzero(r->desc, nentries * 8u);
	dma_sync(r->desc, nentries * 8u);
	r->sw_index = brd(p->base + CE_CURRENT_DRRI) & (nentries - 1u);
	r->hw_index = r->sw_index;
	r->write_index = brd(p->base + CE_DST_WR_INDEX) & (nentries - 1u);
	bwr(p->base + CE_DR_BA, r->phys);
	bwr(p->base + CE_DR_SIZE, nentries);
	bwr(p->base + CE_CTRL1, brd(p->base + CE_CTRL1) & ~(1u << 17));   /* no byte swap */
	bwr(p->base + CE_DST_WATERMARK, nentries & 0xFFFFu);
}

/* _ath10k_ce_send_nolock: addr is a bus address (phone RAM) or a CE-space target address */
static bool
ce_send(struct ce_pipe *p, uint32_t addr, uint32_t nbytes, uint32_t flags)
{
	struct ce_ring *r = &p->src;
	uint32_t mask = r->nentries - 1u;
	if (((r->sw_index - 1u - r->write_index) & mask) == 0) {
		return false;   /* full */
	}
	uint32_t *d = &r->desc[r->write_index * 2u];
	d[0] = addr;
	d[1] = (nbytes & 0xFFFFu) | (flags << 16);
	dma_sync(d, 8u);
	r->write_index = (r->write_index + 1u) & mask;
	bwr(p->base + CE_SR_WR_INDEX, r->write_index);
	return true;
}

/* __ath10k_ce_rx_post_buf */
static bool
ce_post_rx(struct ce_pipe *p, uint32_t addr)
{
	struct ce_ring *r = &p->dst;
	uint32_t mask = r->nentries - 1u;
	if (((r->sw_index - 1u - r->write_index) & mask) == 0) {
		return false;
	}
	uint32_t *d = &r->desc[r->write_index * 2u];
	d[0] = addr;
	d[1] = 0;   /* nbytes 0: the copy engine fills it in on completion */
	dma_sync(d, 8u);
	r->write_index = (r->write_index + 1u) & mask;
	bwr(p->base + CE_DST_WR_INDEX, r->write_index);
	return true;
}

/* _ath10k_ce_completed_send_next_nolock */
static bool
ce_send_done(struct ce_pipe *p)
{
	struct ce_ring *r = &p->src;
	if (r->hw_index == r->sw_index) {
		uint32_t ri = brd(p->base + CE_CURRENT_SRRI);
		if (ri == 0xFFFFFFFFu) {
			return false;
		}
		r->hw_index = ri & (r->nentries - 1u);
	}
	if (r->hw_index == r->sw_index) {
		return false;
	}
	r->sw_index = (r->sw_index + 1u) & (r->nentries - 1u);
	return true;
}

/* _ath10k_ce_completed_recv_next_nolock: done once the copy engine has written a non-zero nbytes. At most one
 * descriptor per ring is outstanding here, so clearing nbytes cannot race the chip on a shared cache line. */
static bool
ce_recv_done(struct ce_pipe *p, uint32_t *nbytes)
{
	struct ce_ring *r = &p->dst;
	uint32_t *d = &r->desc[r->sw_index * 2u];
	dma_sync(d, 8u);
	uint32_t n = d[1] & 0xFFFFu;
	if (n == 0) {
		return false;
	}
	d[1] &= 0xFFFF0000u;
	dma_sync(d, 8u);
	*nbytes = n;
	r->sw_index = (r->sw_index + 1u) & (r->nentries - 1u);
	return true;
}

/* One send on tx and the matching receive on rx. */
static bool
ce_wait(struct ce_pipe *tx, struct ce_pipe *rx, uint32_t *nbytes, uint32_t timeout_us, const char *what)
{
	bool txd = false, rxd = false;
	uint32_t t = 0;
	for (;;) {
		txd = txd || ce_send_done(tx);
		rxd = rxd || ce_recv_done(rx, nbytes);
		if (txd && rxd) {
			return true;
		}
		if (t >= timeout_us) {
			break;
		}
		if (timeout_us > 100000u) {
			IOSleep(1);
			t += 1000u;
		} else {
			IODelay(50);
			t += 50u;
		}
	}
	A_LOG("%s timed out after %u us: send %s, receive %s", what, t, txd ? "done" : "PENDING", rxd ? "done" : "PENDING");
	return false;
}

static void
ce_dump(const char *name, const struct ce_pipe *p)
{
	uint32_t b = p->base;
	A_LOG("%s: sr_ba %08x/%u dr_ba %08x/%u ctrl1 %08x host_is %08x misc_is %08x sr_wr %u srri %u dst_wr %u drri %u",
	    name, brd(b + CE_SR_BA), brd(b + CE_SR_SIZE), brd(b + CE_DR_BA), brd(b + CE_DR_SIZE), brd(b + CE_CTRL1),
	    brd(b + CE_HOST_IS), brd(b + CE_MISC_IS), brd(b + CE_SR_WR_INDEX), brd(b + CE_CURRENT_SRRI),
	    brd(b + CE_DST_WR_INDEX), brd(b + CE_CURRENT_DRRI));
}

/* ---------------------------------------------------------------------------------------------------- */
/* diagnostic window (pci.c ath10k_pci_diag_read_mem / _write_mem), one transfer of <= DIAG_MAX bytes   */

static bool
diag_read(uint32_t taddr, void *out, uint32_t nbytes)
{
	uint8_t *buf = gDma + OFF_DIAG;
	uint32_t got = 0;
	dma_sync(buf, nbytes);   /* no dirty line may later overwrite what the chip writes */
	if (!ce_post_rx(&gCe7, gDmaPhys + OFF_DIAG) || !ce_send(&gCe7, targ_to_ce(taddr), nbytes, 0)) {
		A_LOG("diag read %08x: CE7 ring full", taddr);
		return false;
	}
	if (!ce_wait(&gCe7, &gCe7, &got, 10000u, "diag read")) {
		return false;
	}
	if (got != nbytes) {
		A_LOG("diag read %08x: %u bytes back, asked for %u", taddr, got, nbytes);
		return false;
	}
	dma_sync(buf, nbytes);
	memcpy(out, buf, nbytes);
	return true;
}

static bool
diag_write(uint32_t taddr, const void *in, uint32_t nbytes)
{
	uint8_t *buf = gDma + OFF_DIAG;
	uint32_t got = 0;
	memcpy(buf, in, nbytes);
	dma_sync(buf, nbytes);
	if (!ce_post_rx(&gCe7, targ_to_ce(taddr)) || !ce_send(&gCe7, gDmaPhys + OFF_DIAG, nbytes, 0)) {
		A_LOG("diag write %08x: CE7 ring full", taddr);
		return false;
	}
	if (!ce_wait(&gCe7, &gCe7, &got, 10000u, "diag write")) {
		return false;
	}
	if (got != nbytes) {
		A_LOG("diag write %08x: %u bytes done, asked for %u", taddr, got, nbytes);
		return false;
	}
	return true;
}

static bool
diag_read32(uint32_t taddr, uint32_t *v)
{
	return diag_read(taddr, v, 4u);
}

static bool
diag_write32(uint32_t taddr, uint32_t v)
{
	return diag_write(taddr, &v, 4u);
}

/* ---------------------------------------------------------------------------------------------------- */
/* BMI (pci.c ath10k_pci_hif_exchange_bmi_msg): request on CE0, response on CE1, 3 s                    */

static bool
bmi_exchange(const void *req, uint32_t req_len, void *resp, uint32_t *resp_len)
{
	uint8_t *rq = gDma + OFF_BMI_REQ, *rs = gDma + OFF_BMI_RESP;
	uint32_t got = 0;
	memcpy(rq, req, req_len);
	dma_sync(rq, req_len);
	dma_sync(rs, BMI_RESP_MAX);
	if (!ce_post_rx(&gCe1, gDmaPhys + OFF_BMI_RESP) ||
	    !ce_send(&gCe0, gDmaPhys + OFF_BMI_REQ, req_len, BMI_SEND_FLAGS)) {
		A_LOG("%s", "BMI: ring full");
		return false;
	}
	if (!ce_wait(&gCe0, &gCe1, &got, 3000000u, "BMI exchange")) {
		return false;
	}
	if (got > BMI_RESP_MAX) {
		A_LOG("BMI: %u-byte response overran the %u-byte buffer (inside the DMA block)", got, BMI_RESP_MAX);
		got = BMI_RESP_MAX;
	}
	dma_sync(rs, got);
	*resp_len = got < *resp_len ? got : *resp_len;
	memcpy(resp, rs, *resp_len);
	return true;
}

/* ---------------------------------------------------------------------------------------------------- */

static bool
run(char *res, size_t len)
{
	/* 1. The boot ROM's "initialized" (qcacld-2.0 hif_pci_configure "FW sync": 10 ms polls, 10 s). */
	if (!q_wake()) {
		snprintf(res, len, "STEP 2b FAIL: chip SoC did not wake (rtc_state %08x)", brd(Q_RTC_STATE));
		return false;
	}
	uint32_t fwi = 0xDEADBEEFu, t = 0;
	for (;;) {
		(void)q_wake();
		uint32_t v = brd(Q_FW_INDICATOR);
		if (v != fwi) {
			A_LOG("fw indicator %08x at %u ms (%u ms after WLAN_EN)", v, t, ms_since_power_on());
			fwi = v;
		}
		if ((v != 0xFFFFFFFFu && (v & (Q_FW_IND_INITIALIZED | Q_FW_IND_EVENT_PENDING))) || t >= 10000u) {
			break;
		}
		IOSleep(10);
		t += 10;
	}
	if (fwi == 0xFFFFFFFFu) {
		snprintf(res, len, "%s", "STEP 2b FAIL: BAR0 reads all ones while waiting for the boot ROM");
		return false;
	}
	if (fwi & Q_FW_IND_EVENT_PENDING) {
		snprintf(res, len, "STEP 2b FAIL: chip reports a crash (fw indicator %08x) before any host command", fwi);
		return false;
	}
	bool rom_ready = (fwi & Q_FW_IND_INITIALIZED) != 0;
	A_LOG("boot ROM %s after %u ms", rom_ready ? "initialized" : "NOT initialized", t);

	/* 2. DMA block (below 4 GB: 32-bit descriptors), chip ASPM off, bus mastering on. */
	IOPhysicalAddress phys = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	gDma = (uint8_t *)IOMallocContiguous(DMA_SIZE, 4096, &phys);
#pragma clang diagnostic pop
	if (gDma == NULL || phys == 0 || (uint64_t)phys + DMA_SIZE > 0x100000000ULL) {
		snprintf(res, len, "STEP 2b FAIL: no DMA block below 4 GB (va %p phys %llx)", (void *)gDma,
		    (unsigned long long)phys);
		gDma = NULL;
		return false;
	}
	gDmaPhys = (uint32_t)phys;
	uint32_t win = (brd(Q_CORE_CTRL) & 0x7ffu) << 21;   /* addresses the copy engines keep inside the chip */
	if (gDmaPhys < win + 0x200000u && gDmaPhys + DMA_SIZE > win) {
		snprintf(res, len, "STEP 2b FAIL: DMA block %08x lies in the chip's own 2 MB window at %08x", gDmaPhys, win);
		return false;
	}
	bzero(gDma, DMA_SIZE);
	dma_sync(gDma, DMA_SIZE);
	A_LOG("DMA block %u KB at phys %08x (va %p); core_ctrl %08x", DMA_SIZE / 1024u, gDmaPhys, (void *)gDma,
	    brd(Q_CORE_CTRL));

	uint32_t cap = chip_pcie_cap();
	if (cap != 0) {
		uint32_t lc = rd32(gConf, cap + 0x10u);
		if (lc & 3u) {
			wr32(gConf, cap + 0x10u, lc & 0xFFFCu);   /* ASPM off (ath10k_pci_hif_power_up); 0s to LNKSTA */
		}
		A_LOG("chip PCIe capability at %02x: lnkctl %04x -> %04x", cap, lc & 0xFFFFu, rd32(gConf, cap + 0x10u) & 0xFFFFu);
	}
	wr32(gDbi, 0x04, (rd32(gDbi, 0x04) & 0xFFFFu) | 0x6u);     /* root port: memory + bus master */
	wr32(gConf, 0x04, (rd32(gConf, 0x04) & 0xFFFFu) | 0x6u);   /* chip: memory + bus master */
	A_LOG("bus mastering on: rp command %08x, chip command %08x", rd32(gDbi, 0x04), rd32(gConf, 0x04));

	/* 3. Copy engines, host side (pci_host_ce_config_wlan; fewer CE1 entries: one BMI response at a time). */
	gCe0.base = Q_CE(0);
	gCe1.base = Q_CE(1);
	gCe7.base = Q_CE(7);
	ce_quiet(&gCe0);
	ce_quiet(&gCe1);
	ce_quiet(&gCe7);
	ce_init_src(&gCe0, OFF_CE0_SRC, 16u, BMI_REQ_MAX);
	ce_init_dst(&gCe1, OFF_CE1_DST, 16u);
	ce_init_src(&gCe7, OFF_CE7_SRC, 2u, DIAG_MAX);
	ce_init_dst(&gCe7, OFF_CE7_DST, 2u);
	ce_dump("CE0", &gCe0);
	ce_dump("CE1", &gCe1);
	ce_dump("CE7", &gCe7);

	/* 4. First DMA, both directions: CE7 reads our descriptors and writes the answer into our buffer. */
	uint32_t pcie_state = 0;
	if (!diag_read32(HI_INTERCONNECT_STATE, &pcie_state)) {
		ce_dump("CE7", &gCe7);
		snprintf(res, len, "%s", "STEP 2b FAIL: diag read of hi_interconnect_state (first DMA, CE7) timed out");
		return false;
	}
	A_LOG("diag read hi_interconnect_state (target %08x = ce %08x): pcie_state at %08x", HI_INTERCONNECT_STATE,
	    targ_to_ce(HI_INTERCONNECT_STATE), pcie_state);
	if (!rom_ready) {
		snprintf(res, len, "STEP 2b PARTIAL: DMA works (pcie_state %08x), but the boot ROM never reported "
		    "initialized in 10 s; BMI not tried", pcie_state);
		return false;
	}
	if (pcie_state == 0 || pcie_state == 0xFFFFFFFFu) {
		snprintf(res, len, "STEP 2b FAIL: DMA works but hi_interconnect_state is %08x", pcie_state);
		return false;
	}

	/* 5. ath10k_pci_init_config, then ath10k_pci_wake_target_cpu. */
	uint32_t pipe_cfg = 0, svc_map = 0, cfg_flags = 0, ealloc = 0, flag2 = 0;
	bool ok = diag_read32(pcie_state + PCIE_STATE_PIPE_CFG, &pipe_cfg) &&
	    diag_read32(pcie_state + PCIE_STATE_SVC_MAP, &svc_map) &&
	    diag_read32(pcie_state + PCIE_STATE_CONFIG_FLAGS, &cfg_flags) &&
	    diag_read32(HI_EARLY_ALLOC, &ealloc) &&
	    diag_read32(HI_OPTION_FLAG2, &flag2);
	A_LOG("pcie_state: pipe_cfg %08x svc_map %08x config_flags %08x; hi_early_alloc %08x hi_option_flag2 %08x",
	    pipe_cfg, svc_map, cfg_flags, ealloc, flag2);
	if (!ok || pipe_cfg == 0 || svc_map == 0) {
		snprintf(res, len, "STEP 2b FAIL: target config pointers (pipe_cfg %08x svc_map %08x)%s", pipe_cfg, svc_map,
		    ok ? "" : ", a diag read timed out");
		return false;
	}
	ok = diag_write(pipe_cfg, kTargetCeConfig, sizeof(kTargetCeConfig)) &&
	    diag_write(svc_map, kServiceMap, sizeof(kServiceMap)) &&
	    diag_write32(pcie_state + PCIE_STATE_CONFIG_FLAGS, cfg_flags & ~PCIE_CONFIG_FLAG_ENABLE_L1) &&
	    diag_write32(HI_EARLY_ALLOC, ealloc | (HI_EARLY_ALLOC_MAGIC << 16) | (HW3_IRAM_BANKS & 0xFu)) &&
	    diag_write32(HI_OPTION_FLAG2, flag2 | HI_OPTION_EARLY_CFG_DONE);
	uint32_t back[6] = { 0, 0, 0, 0, 0, 0 }, flag2_back = 0;
	bool verified = ok && diag_read(pipe_cfg, back, sizeof(back)) && diag_read32(HI_OPTION_FLAG2, &flag2_back) &&
	    memcmp(back, kTargetCeConfig[0], sizeof(back)) == 0 && (flag2_back & HI_OPTION_EARLY_CFG_DONE);
	A_LOG("target config written: %s (pipe 0 reads back %u/%u/%u/%u, hi_option_flag2 %08x)", verified ? "verified" : "NOT verified",
	    back[0], back[1], back[2], back[3], flag2_back);
	if (!verified) {
		snprintf(res, len, "STEP 2b FAIL: target config %s", ok ? "did not read back" : "diag write timed out");
		return false;
	}
	bwr(Q_CORE_CTRL, brd(Q_CORE_CTRL) | Q_CORE_CTRL_CPU_INTR);

	/* 6. BMI_GET_TARGET_INFO (ath10k_bmi_get_target_info): 4-byte command, response { len, version, type }. */
	uint32_t cmd = BMI_GET_TARGET_INFO, rsp[3] = { 0, 0, 0 }, rlen = sizeof(rsp);
	if (!bmi_exchange(&cmd, sizeof(cmd), rsp, &rlen)) {
		ce_dump("CE0", &gCe0);
		ce_dump("CE1", &gCe1);
		snprintf(res, len, "STEP 2b FAIL: no answer to BMI_GET_TARGET_INFO in 3 s (fw indicator %08x); DMA, diag and "
		    "target config OK", brd(Q_FW_INDICATOR));
		return false;
	}
	A_LOG("BMI_GET_TARGET_INFO: %u bytes: len %u version %08x type %u", rlen, rsp[0], rsp[1], rsp[2]);
	if (rlen < sizeof(rsp)) {
		snprintf(res, len, "STEP 2b FAIL: BMI_GET_TARGET_INFO answered %u bytes, expected 12", rlen);
		return false;
	}
	snprintf(res, len, "%s: BMI target version %08x type %u (hw3.2 = %08x); DMA, diag window, target config OK",
	    rsp[1] == QCA6174_HW_3_2_VERSION ? "STEP 2b PASS" : "STEP 2b: unexpected target version", rsp[1], rsp[2],
	    QCA6174_HW_3_2_VERSION);
	return rsp[1] == QCA6174_HW_3_2_VERSION;
}

extern "C" bool
op2_ath10k_step2b(const struct op2_ath10k_bus *bus, char *result, size_t len)
{
	gB = bus->bar0;
	gConf = bus->conf;
	gDbi = bus->dbi;
	gPowerOnAbs = bus->power_on_abs;
	bool ok = run(result, len);
	/* The chip cannot DMA any more; the block stays allocated for good. The chip waits in BMI. */
	wr32(gConf, 0x04, rd32(gConf, 0x04) & 0xFFFBu);
	wr32(gDbi, 0x04, rd32(gDbi, 0x04) & 0xFFFBu);
	bwr(Q_SOC_WAKE, 0u);
	A_LOG("done: bus mastering off (chip command %08x, rp command %08x), SoC wake released", rd32(gConf, 0x04),
	    rd32(gDbi, 0x04));
	return ok;
}

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
