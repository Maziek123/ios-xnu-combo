/*
 * OP2Wifi: Wi-Fi step 1 (docs/WIFI-BT-PLAN.md, P0) for the OnePlus 2 (MSM8994).
 *
 * The Wi-Fi/Bluetooth chip is a QCA6174 on PCIe root complex 1 (not WCNSS). This file powers the chip and
 * brings RC1's link up far enough to read the chip's PCI vendor/device ID (expect 168c:003e). No driver,
 * no DMA, no interrupts, no firmware: success is one log line.
 *
 * Boot-arg wifi:
 *   0  (default) nothing runs
 *   1  read-only probe: log GPIO / PMIC / clock / power-domain states; writes nothing
 *   2  step 1: power + clocks + PHY + link training + config read; stops once the chip answers on the link
 *   3  as 2, then step 2a: give BAR0 an address, enable memory decoding (no bus mastering: the chip
 *      cannot DMA yet), wake the chip's SoC, wait up to 5 s for it to initialise (the chip ID reads 0 until
 *      then), and read its chip ID and firmware indicator (ath10k pci.c / hw.c qca6174_regs). Only if the
 *      ID is still 0: the ath10k cold reset (SOC_GLOBAL_RESET) and a 3 s wait for the boot ROM's "initialized"
 *   4  as 3, then step 2b (OP2Ath10k.cpp): DMA through the copy engines, the diagnostic window into the
 *      chip's memory, the target-side config, and the first boot-loader (BMI) command; no firmware yet
 *   5  as 4, then step 2c (OP2Ath10k.cpp): board data, OTP and firmware download, BMI_DONE, and the firmware's
 *      HTC ready message (firmware embedded from the gitignored firmware/qca6174/ by tools/gen-wifi-fw.py)
 *   6  as 5, then step 3 (OP2Ath10k.cpp): HTC services (HTT data, WMI control), WMI service ready, WMI init,
 *      WMI ready with the chip's MAC address, HTT version, a WMI echo
 *   7  as 6, then step 4 (OP2Ath10k.cpp): a scan that only listens; the log lists the networks heard
 *   8  as 7, then step 5 (OP2Ath10k.cpp): join the network named in the gitignored firmware/qca6174/wifi-network.txt
 *      (authentication, association), then leave it
 *   9  as 8, then step 6 (OP2Ath10k.cpp): the WPA2-PSK 4-way handshake, keys into the chip, 5 s connected, leave
 *  10  as 9, then step 7 (OP2Ath10k.cpp): data through the keys: DHCP with the network, ARP and ping its router, leave
 *  11  as 10, then step 8 (OP2Ath10k.cpp + bsd/dev/op2_wlan.c): wl0, a network interface for iOS on that lease;
 *      stays connected until the network ends it or the phone resets (op2shell publishes it to configd)
 *  12  steps 1-4, then the Wi-Fi manager: scan and join at runtime as op2netd's page asks (net.op2wlan_cmd),
 *      the build's network (if any) first; steps 5-8 run for each join
 *
 * Runs in its own kernel thread 120 s after the framebuffer starts (RPM is up by then, the UI is running),
 * and repeats a one-line summary every 60 s so it survives in the ramoops ring.
 *
 * Boot-arg bt (Bluetooth: the same chip over a UART, BLSP2 UART2 at 0xf995e000 = Linux's ttyHS0, pins 45-48;
 * reset = PM8994 GPIO 19; supplies bt_vreg, S4, L30 as Wi-Fi), run first in the same thread, lines "OP2 BT:":
 *   0  (default) nothing
 *   1  read-only probe: the UART pins, the reset line, the supplies, the UART's clocks (its registers only if
 *      they run: an unclocked UART hangs the bus)
 *   2  step 1: supplies on; the chip's side of the UART watched on the pins (as plain inputs): its TX (our RX, pin
 *      46) high = idle, its RTS (our CTS, pin 47) low = ready. First 200 ms as found (the first run, 2026-11-27,
 *      found the reset line already high and RX at 1: the chip running), then reset low 50 ms, high, and 5 s of
 *      watching. No UART clock, no data: success is one line. The chip is put back in reset. (A retry right after
 *      Wi-Fi raised WLAN_EN, 2026-11-27, saw nothing either and broke Wi-Fi's PCIe link training: removed.)
 *   3  step 2: talk to it. The UART clocked (BLSP2 AHB vote, UART2 RCG 7.3728 MHz from GPLL0 = M/N 192/15625, branch on),
 *      pins 45-48 on function 2, UART_DM v1.4 (msm_serial_hs_hwreg.h offsets) at 115200 8N1 (CSR 0xCC = /4), FIFO mode,
 *      polled, our RFR asserted by command (no automatic flow control). With the chip held in reset first, then
 *      released: whatever it sends by itself, then Qualcomm's "get version" (01 00 fc 01 19, what Linux's hci_qca
 *      sends first) and HCI Reset (01 03 0c 00; the answer is 04 0e 04 01 03 0c 00), each with 500 ms for the
 *      answer, logged in hex. Back in reset at the end. Runs instead of step 1.
 *
 * Sequence and values from the OnePlus msm8994 kernel (OnePlus2_14_A.12_160226):
 *   drivers/pci/host/pci-msm.c       msm_pcie_enable(), pcie_phy_init() (20nm QMP, 19.2 MHz),
 *                                    msm_pcie_iatu_config(), msm_pcie_vreg_info
 *   drivers/net/wireless/cnss/cnss.c power-on: vregs, 2 ms, bootstrap high, 10 ms, WLAN_EN high, 10 ms
 *   drivers/clk/qcom/clock-gcc-8994.c PCIE_1_* clock/reset offsets, aux 1.011 MHz, pipe 125 MHz
 *   arch/arm64/boot/dts/14049_HW_14/msm8994.dtsi pcie1 node; research/hardware/live.dts (supplies, pins)
 * Register values are facts re-typed here, not copied code.
 */

#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_MSM8994)

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
#include "op2_rpm.h"
#include "op2_ath10k.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" bool op2_spmi_read(uint8_t sid, uint16_t addr, uint8_t *buf, uint32_t len);   /* OP2Battery.cpp */
extern "C" bool op2_spmi_write(uint8_t sid, uint16_t addr, const uint8_t *buf, uint32_t len);
extern "C" void op2_wifi_start(void);   /* called from op2_threaddump_start (osfmk/arm64/op2_snapshot.c) */

/* ---- physical addresses ---- */
#define W_GCC_PHYS        0xfc400000ULL
#define W_TLMM_PHYS       0xfd510000ULL
#define W_PARF_PHYS       0xfc528000ULL   /* 0x2000 */
#define W_PHY_PHYS        0xfc52e000ULL   /* 0x1000 */
#define W_DBI_PHYS        0xf8800000ULL   /* dm_core 0xf1d, ELBI at +0xf20 */
#define W_CONF_PHYS       0xf8801000ULL   /* config window, 4 KB used */
#define W_BARS_PHYS       0xf8900000ULL   /* "bars": 7 MB memory window, passed to PCIe untranslated */
#define W_BARS_SIZE       0x700000ULL

/* ---- QCA6174 BAR0 (ath10k hw.c qca6174_regs, hw.h, pci.c) ---- */
#define Q_RTC_SOC_BASE      0x00000800
#define Q_SOC_RESET_CONTROL (Q_RTC_SOC_BASE + 0x00)        /* read-only here (warm reset bits: step 2b) */
#define Q_SOC_LF_TIMER0     (Q_RTC_SOC_BASE + 0x50)
#define Q_SOC_CHIP_ID       (Q_RTC_SOC_BASE + 0xf0)        /* 0 until the chip has initialised */
#define Q_PCIE_LOCAL_BASE   0x00080000
#define Q_RTC_STATE         (Q_PCIE_LOCAL_BASE + 0x0000)   /* bits 2:0 == 3: SoC awake */
#define Q_SOC_WAKE          (Q_PCIE_LOCAL_BASE + 0x0004)   /* 1 = request wake, 0 = release */
#define Q_SOC_GLOBAL_RESET  (Q_PCIE_LOCAL_BASE + 0x0008)   /* bit 0: whole chip, PCIe core included, in reset */
#define Q_SOC_CORE_BASE     0x0003a000
#define Q_PCIE_INTR_ENABLE  (Q_SOC_CORE_BASE + 0x08)
#define Q_PCIE_INTR_CAUSE   (Q_SOC_CORE_BASE + 0x0c)
#define Q_FW_INDICATOR      (Q_SOC_CORE_BASE + 0x28)       /* 0x3a028 */
#define Q_FW_IND_EVENT_PENDING 0x1u                        /* target crashed */
#define Q_FW_IND_INITIALIZED   0x2u                        /* boot ROM / firmware ready */
#define Q_HW32_CHIP_REV     10                             /* QCA6174_HW_3_2_CHIP_ID_REV */

/* ---- GCC (clock-gcc-8994.c) ---- */
#define GCC_PCIE_1_GDSCR          0x1B44   /* msm-gdsc.dtsi gdsc_pcie_1 */
#define GCC_PCIE_1_SLV_AXI_CBCR   0x1B48
#define GCC_PCIE_1_MSTR_AXI_CBCR  0x1B4C
#define GCC_PCIE_1_CFG_AHB_CBCR   0x1B50
#define GCC_PCIE_1_AUX_CBCR       0x1B54
#define GCC_PCIE_1_PIPE_CBCR      0x1B58
#define GCC_PCIE_1_PIPE_CMD_RCGR  0x1B5C
#define GCC_PCIE_1_AUX_CMD_RCGR   0x1B80
#define GCC_PCIE_PHY_1_PHY_BCR    0x1B94   /* pipe clock reset */
#define GCC_PCIE_PHY_1_BCR        0x1B98   /* gcc_pcie_phy_1_reset */
#define GCC_PCIE_1_PHY_LDO_EN     0x1E04
#define CBCR_EN                   (1u << 0)
#define CBCR_OFF                  (1u << 31)
#define GDSC_SW_COLLAPSE          (1u << 0)
#define GDSC_PWR_ON               (1u << 31)

/* ---- TLMM (8974-family layout, as OP2I2C) ---- */
#define TLMM_CFG(p)     (0x1000 + (p) * 0x10)
#define TLMM_IO(p)      (0x1004 + (p) * 0x10)
#define PIN_PERST       35
#define PIN_WAKE        37
#define PIN_BOOTSTRAP   112
#define PIN_WLAN_EN     113

/* ---- Bluetooth (live.dts bt_qca6174, uart@f995e000 = blsp2_uart2, gcc-msm8994.c) ---- */
#define PIN_BT_TX       45        /* blsp2_uart2_active: pins 45-48, function 2 (TX, RX, CTS, RFR) */
#define PIN_BT_RX       46
#define PIN_BT_CTS      47
#define PIN_BT_RFR      48
#define GCC_BLSP2_AHB_CBCR        0x0944   /* voted: APCS_CLOCK_BRANCH_ENA_VOTE bit 15 */
#define GCC_APCS_BRANCH_VOTE      0x1484
#define GCC_BLSP2_UART2_APPS_CBCR 0x0A44
#define GCC_BLSP2_UART2_CMD_RCGR  0x0A4C   /* CMD, CFG, M, N, D */
#define W_BTUART_PHYS   0xf995e000ULL       /* UART_DM v1.4 (msm_serial_hs) */
#define UDM_MR1         0x00
#define UDM_MR2         0x04
#define UDM_DMRX        0x34
#define UDM_DMEN        0x3C
#define UDM_NCF_TX      0x40
#define UDM_TXFS        0x4C
#define UDM_RXFS        0x50
#define UDM_CSR         0xA0
#define UDM_SR          0xA4
#define UDM_CR          0xA8
#define UDM_IMR         0xB0
#define UDM_ISR         0xB4
#define UDM_TF          0x100
#define UDM_RF          0x140
#define UDM_SR_RXRDY    (1u << 0)
#define UDM_SR_TXRDY    (1u << 2)
#define UDM_SR_TXEMT    (1u << 3)
#define UDM_ISR_TXREADY (1u << 7)
#define UDM_ISR_CTS     (1u << 6)   /* the current state of our CTS input = the chip's RTS */
#define UDM_CR_RX_EN    (1u << 0)
#define UDM_CR_RX_DIS   (1u << 1)
#define UDM_CR_TX_EN    (1u << 2)
#define UDM_CR_TX_DIS   (1u << 3)
#define UDM_CMD_RESET_RX        0x10
#define UDM_CMD_RESET_TX        0x20
#define UDM_CMD_RESET_ERR       0x30
#define UDM_CMD_RESET_BRK       0x40
#define UDM_CMD_RESET_CTS       0x70
#define UDM_CMD_RESET_STALE     0x80
#define UDM_CMD_RFR_LOW         0xD0   /* RFR asserted: the chip may send */
#define UDM_CMD_CLEAR_TX_READY  0x300
#define UDM_CMD_FORCE_STALE     0x400
#define UDM_CMD_STALE_ENABLE    0x500

/* ---- PM8994 (SPMI SID 0) ---- */
#define PM_SID          0
#define PMGPIO9_BASE    0xC800    /* bt_vreg: regulator-fixed, gpio = <&pm8994_gpios 9>, active high */
#define PMGPIO19_BASE   0xD200    /* bt_qca6174 qca,bt-reset-gpio: low = in reset; mode 1, vin-sel 2, invert 0 */
#define PMLDO(n)        ((uint16_t)(0x4000 + ((n) - 1) * 0x100))
#define PM_STATUS1      0x08
#define PM_MODE_CTL     0x40
#define PM_DIG_VIN_CTL  0x41
#define PM_DIG_OUT_CTL  0x45
#define PM_EN_CTL       0x46

/* ---- RPM ---- */
#define RPM_CLKA        0x616b6c63u   /* "clka": XO buffers */
#define RPM_LN_BB_ID    8u            /* clock-rpm-8994.c LN_BB_CLK_ID: PCIe 19.2 MHz reference */
#define RPM_KEY_MA      0x0000616du   /* "ma": load current, mA (selects HPM) */

/* ---- PCIe (pci-msm.c) ---- */
#define PARF_SYS_CTRL        0x000
#define PARF_PHY_CTRL        0x040
#define PARF_DBI_BASE_ADDR   0x168
#define PARF_LTSSM           0x1B0
#define ELBI_SYS_STTS        0xF28    /* ELBI (0xf20) + 0x08, in the DBI page */
#define XMLH_LINK_UP         0x400
#define DBI_DEVCTL_STATUS    0x078
#define DBI_LINK_STATUS      0x080    /* bit 29: data link layer active */
#define IATU_VIEWPORT        0x900
#define IATU_CTRL1           0x904
#define IATU_CTRL2           0x908
#define IATU_LBAR            0x90C
#define IATU_UBAR            0x910
#define IATU_LAR             0x914
#define IATU_LTAR            0x918
#define IATU_UTAR            0x91C
#define PHY_SW_RESET         0x600
#define PHY_POWER_DOWN       0x604
#define PHY_START            0x608
#define PHY_PCS_STATUS       0x728    /* bit 6 set = PHY not ready */

static volatile uint32_t *gGcc, *gTlmm, *gParf, *gPhy, *gDbi, *gConf, *gBar0;
static uint32_t gMode, gBtMode;
static char gSummary[320] = "not run";

#define W_LOG(fmt, ...) do { IOLog("OP2 WIFI: " fmt "\n", __VA_ARGS__); op2_keep("OP2 WIFI: " fmt, __VA_ARGS__); } while (0)
#define W_LOG_CONSOLE(fmt, ...) IOLog("OP2 WIFI: " fmt "\n", __VA_ARGS__)   /* console only */
#define B_LOG(fmt, ...) do { IOLog("OP2 BT: " fmt "\n", __VA_ARGS__); op2_keep("OP2 BT: " fmt, __VA_ARGS__); } while (0)

static inline uint32_t rd(volatile uint32_t *b, uint32_t off) { return b[off / 4]; }
static inline void wr(volatile uint32_t *b, uint32_t off, uint32_t v)
{
	b[off / 4] = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
summary(const char *s)
{
	strlcpy(gSummary, s, sizeof(gSummary));
	W_LOG("RESULT: %s", gSummary);
}

/* ---------------------------------------------------------------------------------------------------- */
/* read-only state dump (wifi=1, and before/after each bring-up stage)                                  */

static void
dump_pin(const char *name, uint32_t p)
{
	uint32_t cfg = rd(gTlmm, TLMM_CFG(p)), io = rd(gTlmm, TLMM_IO(p));
	W_LOG("pin %u %-9s cfg=0x%03x (func %u drv %u pull %u oe %u) in=%u out=%u", p, name, cfg,
	    (cfg >> 2) & 0xf, (cfg >> 6) & 7, cfg & 3, (cfg >> 9) & 1, io & 1, (io >> 1) & 1);
}

static void
dump_pmic(const char *name, uint16_t base)
{
	uint8_t st = 0, mode = 0, vin = 0, out = 0, en = 0;
	bool ok = op2_spmi_read(PM_SID, (uint16_t)(base + PM_STATUS1), &st, 1) &&
	    op2_spmi_read(PM_SID, (uint16_t)(base + PM_MODE_CTL), &mode, 1) &&
	    op2_spmi_read(PM_SID, (uint16_t)(base + PM_DIG_VIN_CTL), &vin, 1) &&
	    op2_spmi_read(PM_SID, (uint16_t)(base + PM_DIG_OUT_CTL), &out, 1) &&
	    op2_spmi_read(PM_SID, (uint16_t)(base + PM_EN_CTL), &en, 1);
	if (!ok) {
		W_LOG("pmic %-8s 0x%04x: SPMI read failed", name, base);
		return;
	}
	W_LOG("pmic %-8s 0x%04x: status1 %02x mode/vset %02x %02x out %02x en %02x", name, base, st, mode, vin, out, en);
}

static void
dump_clocks(const char *when)
{
	W_LOG("%s: gdsc %08x ahb %08x mstr %08x slv %08x aux %08x pipe %08x ldo %08x physbcr %08x pipebcr %08x", when,
	    rd(gGcc, GCC_PCIE_1_GDSCR), rd(gGcc, GCC_PCIE_1_CFG_AHB_CBCR), rd(gGcc, GCC_PCIE_1_MSTR_AXI_CBCR),
	    rd(gGcc, GCC_PCIE_1_SLV_AXI_CBCR), rd(gGcc, GCC_PCIE_1_AUX_CBCR), rd(gGcc, GCC_PCIE_1_PIPE_CBCR),
	    rd(gGcc, GCC_PCIE_1_PHY_LDO_EN), rd(gGcc, GCC_PCIE_PHY_1_BCR), rd(gGcc, GCC_PCIE_PHY_1_PHY_BCR));
}

/* PCIe registers may only be read with the controller powered and clocked, or the bus hangs */
static bool
pcie_clocked(void)
{
	return (rd(gGcc, GCC_PCIE_1_GDSCR) & GDSC_PWR_ON) &&
	    !(rd(gGcc, GCC_PCIE_1_CFG_AHB_CBCR) & CBCR_OFF) &&
	    !(rd(gGcc, GCC_PCIE_1_SLV_AXI_CBCR) & CBCR_OFF) &&
	    !(rd(gGcc, GCC_PCIE_1_MSTR_AXI_CBCR) & CBCR_OFF);
}

static void
dump_pcie(const char *when)
{
	if (!pcie_clocked()) {
		W_LOG("%s: controller not powered/clocked: PCIe registers not read", when);
		return;
	}
	W_LOG("%s: parf sys %08x phyctrl %08x ltssm %08x | phy pcs %08x pwrdn %08x | elbi stts %08x dbi id %08x link %08x",
	    when, rd(gParf, PARF_SYS_CTRL), rd(gParf, PARF_PHY_CTRL), rd(gParf, PARF_LTSSM), rd(gPhy, PHY_PCS_STATUS),
	    rd(gPhy, PHY_POWER_DOWN), rd(gDbi, ELBI_SYS_STTS), rd(gDbi, 0x000), rd(gDbi, DBI_LINK_STATUS));
}

static void
dump_all(const char *when)
{
	dump_pin("perst", PIN_PERST);
	dump_pin("wake", PIN_WAKE);
	dump_pin("bootstrap", PIN_BOOTSTRAP);
	dump_pin("wlan_en", PIN_WLAN_EN);
	/* OnePlus switch_antenna (live.dts; drivers/input/misc/switch_antenna.c drives all four low at probe): read only */
	dump_pin("antenna1", 120);
	dump_pin("antenna2", 121);
	dump_pin("antenna3", 122);
	dump_pin("antenna4", 124);
	dump_pmic("bt_vreg", PMGPIO9_BASE);
	dump_pmic("L12", PMLDO(12));
	dump_pmic("L28", PMLDO(28));
	dump_pmic("L30", PMLDO(30));
	dump_clocks(when);
	dump_pcie(when);
}

/* ---------------------------------------------------------------------------------------------------- */
/* bring-up helpers                                                                                     */

/* GPIO function 0, output; drv: 0 = 2 mA .. 7 = 16 mA; pull 1 = down */
static void
pin_out(uint32_t p, uint32_t drv, bool high)
{
	wr(gTlmm, TLMM_IO(p), high ? 2u : 0u);
	wr(gTlmm, TLMM_CFG(p), (1u << 9) | ((drv & 7) << 6) | 1u);
	wr(gTlmm, TLMM_IO(p), high ? 2u : 0u);
}

static bool
ldo_on(uint32_t n, uint32_t uv, uint32_t ma)
{
	uint32_t key = RPM_KEY_MA, val = ma;
	bool ok = op2_rpm_set_voltage(OP2_RPM_RSC_LDOA, n, uv) &&
	    (ma == 0 || op2_rpm_request(OP2_RPM_SET_ACTIVE, OP2_RPM_RSC_LDOA, n, &key, &val, 1)) &&
	    op2_rpm_set_enable(OP2_RPM_RSC_LDOA, n, true);
	W_LOG("RPM L%u %u uV %u mA on: %s", n, uv, ma, ok ? "acked" : "FAILED");
	return ok;
}

static bool
branch_on(uint32_t off, const char *name, bool poll)
{
	wr(gGcc, off, rd(gGcc, off) | CBCR_EN);
	if (!poll) {
		IODelay(100);
		return true;
	}
	for (int i = 0; i < 200; i++) {
		if (!(rd(gGcc, off) & CBCR_OFF)) {
			return true;
		}
		IODelay(5);
	}
	W_LOG("clock %s did not start (cbcr %08x)", name, rd(gGcc, off));
	return false;
}

static bool
rcg_update(uint32_t cmd)
{
	wr(gGcc, cmd, rd(gGcc, cmd) | 1u);
	for (int i = 0; i < 500; i++) {
		if (!(rd(gGcc, cmd) & 1u)) {
			return true;
		}
		IODelay(1);
	}
	W_LOG("RCG 0x%04x update stuck (cmd %08x)", cmd, rd(gGcc, cmd));
	return false;
}

/* SCM restore_sec_cfg(12, 0): pci-msm calls it on every enable when qcom,scm-dev-id is set */
static int32_t
scm_restore_sec_cfg(uint32_t dev_id, uint32_t *scm_ret)
{
	register uint32_t r0 __asm__("w0") = 0x02000C02u;   /* SIP, SCM_SVC_MP 0xC, RESTORE_SEC_CFG 2 */
	register uint32_t r1 __asm__("w1") = 2u;            /* arginfo: 2 values */
	register uint32_t r2 __asm__("w2") = dev_id;
	register uint32_t r3 __asm__("w3") = 0u;
	register uint32_t r4 __asm__("w4") = 0u;
	register uint32_t r5 __asm__("w5") = 0u;
	register uint32_t r6 __asm__("w6") = 0u;
	__asm__ volatile("smc #0\n"
	    : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)
	    : "r"(r4), "r"(r5), "r"(r6)
	    : "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "cc", "memory");
	*scm_ret = r1;
	return (int32_t)r0;
}

/* 20nm QMP PHY, 19.2 MHz reference (pci-msm.c pcie_phy_init, non-FSM9010 branch, vbg_opt unset) */
static const uint32_t kPhyInit[][2] = {
	{ 0x604, 0x03 },  /* POWER_DOWN_CONTROL */
	{ 0x048, 0x08 },  /* COM_SYSCLK_EN_SEL_TXBAND */
	{ 0x0AC, 0x82 },  /* COM_DEC_START1 */
	{ 0x10C, 0x03 },  /* COM_DEC_START2 */
	{ 0x100, 0xD5 },  /* COM_DIV_FRAC_START1 */
	{ 0x104, 0xAA },  /* COM_DIV_FRAC_START2 */
	{ 0x108, 0x4D },  /* COM_DIV_FRAC_START3 */
	{ 0x09C, 0x03 },  /* COM_PLLLOCK_CMP_EN */
	{ 0x090, 0x06 },  /* COM_PLLLOCK_CMP1 */
	{ 0x094, 0x1A },  /* COM_PLLLOCK_CMP2 */
	{ 0x114, 0x7C },  /* COM_PLL_CRCTRL */
	{ 0x034, 0x1F },  /* COM_PLL_CP_SETI */
	{ 0x038, 0x12 },  /* COM_PLL_IP_SETP */
	{ 0x03C, 0x0F },  /* COM_PLL_CP_SETP */
	{ 0x024, 0x01 },  /* COM_PLL_IP_SETI */
	{ 0x00C, 0x0F },  /* COM_IE_TRIM */
	{ 0x010, 0x0F },  /* COM_IP_TRIM */
	{ 0x014, 0x46 },  /* COM_PLL_CNTRL */
	{ 0x400, 0xF4 },  /* RX_CDR_CONTROL1 */
	{ 0x408, 0x2C },  /* RX_CDR_CONTROL_HALF */
	{ 0x004, 0xE1 },  /* COM_PLL_VCOTAIL_EN */
	{ 0x04C, 0x91 },  /* COM_RESETSM_CNTRL */
	{ 0x050, 0x07 },  /* COM_RESETSM_CNTRL2 */
	{ 0x0E0, 0x20 },  /* COM_RES_CODE_START_SEG1 */
	{ 0x0E8, 0x77 },  /* COM_RES_CODE_CAL_CSR */
	{ 0x0F0, 0x15 },  /* COM_RES_TRIM_CONTROL */
	{ 0x268, 0x03 },  /* TX_RCV_DETECT_LVL */
	{ 0x414, 0x09 },  /* RX_UCDR_FO_GAIN */
	{ 0x418, 0x04 },  /* RX_UCDR_SO_GAIN */
	{ 0x41C, 0x49 },  /* RX_UCDR_SO_SATURATION_AND_ENABLE */
	{ 0x4A8, 0xFF },  /* RX_RX_EQ_GAIN1_LSB */
	{ 0x4AC, 0x1F },  /* RX_RX_EQ_GAIN1_MSB */
	{ 0x4B0, 0xFF },  /* RX_RX_EQ_GAIN2_LSB */
	{ 0x4B4, 0x00 },  /* RX_RX_EQ_GAIN2_MSB */
	{ 0x4BC, 0x1E },  /* RX_RX_EQU_ADAPTOR_CNTRL2 */
	{ 0x4F0, 0x67 },  /* RX_RX_EQ_OFFSET_ADAPTOR_CNTRL1 */
	{ 0x4F4, 0x80 },  /* RX_RX_OFFSET_ADAPTOR_CNTRL2 */
	{ 0x4F8, 0x40 },  /* RX_SIGDET_ENABLES */
	{ 0x500, 0xB0 },  /* RX_SIGDET_CNTRL */
	{ 0x504, 0x06 },  /* RX_SIGDET_DEGLITCH_CNTRL */
	{ 0x110, 0x10 },  /* COM_PLL_RXTXEPCLK_EN */
	{ 0x648, 0x10 },  /* PHY_ENDPOINT_REFCLK_DRIVE */
	{ 0x650, 0xA3 },  /* PHY_POWER_STATE_CONFIG1 */
	{ 0x654, 0x4B },  /* PHY_POWER_STATE_CONFIG2 */
	{ 0x64C, 0x4D },  /* PHY_RX_IDLE_DTCT_CNTRL */
	{ 0x600, 0x00 },  /* PHY_SW_RESET */
	{ 0x608, 0x03 },  /* PHY_START */
};

/* ---------------------------------------------------------------------------------------------------- */
/* step 2a: BAR0 + chip registers (link already up, iATU region 0 = config window of 01:00.0)           */

static uint64_t gPowerOnAbs;   /* when WLAN_EN went high */
static uint32_t gBar0Lo;   /* BAR0 low dword as programmed (address | flag bits), for the cold reset's repair */
static bool gBar64;

static uint32_t
ms_since_power_on(void)
{
	uint64_t now = 0, ns = 0;
	clock_get_uptime(&now);
	absolutetime_to_nanoseconds(now - gPowerOnAbs, &ns);
	return (uint32_t)(ns / 1000000u);
}

/* RC side only (ELBI/DBI registers): safe to read whatever the link is doing. */
static bool
rc_link_up(void)
{
	return (rd(gDbi, ELBI_SYS_STTS) & XMLH_LINK_UP) && (rd(gDbi, DBI_LINK_STATUS) & (1u << 29));
}

static bool
wait_rc_link(uint32_t ms)
{
	for (uint32_t i = 0; i < ms && !rc_link_up(); i++) {
		IOSleep(1);
	}
	return rc_link_up();
}

/* Ask the SoC to stay awake; wait for RTC_STATE "on" (ath10k_pci_wake_wait: 30 ms limit). */
static bool
q_wake(uint32_t *rtc)
{
	wr(gBar0, Q_SOC_WAKE, 1u);
	uint32_t st = 0;
	for (int us = 0; us < 30000; us += 50) {
		st = rd(gBar0, Q_RTC_STATE);
		if ((st & 7u) == 3u) {
			break;
		}
		IODelay(50);
	}
	*rtc = st;
	return (st & 7u) == 3u;
}

/* Registers ath10k / qcacld read anyway; reading has no side effects. */
static void
q_snapshot(const char *when)
{
	W_LOG("%s: rtc_state %08x soc_wake %08x global_reset %08x | reset_ctl %08x lf_timer0 %08x chip_id %08x | "
	    "fw_ind %08x intr_en %08x intr_cause %08x", when, rd(gBar0, Q_RTC_STATE), rd(gBar0, Q_SOC_WAKE),
	    rd(gBar0, Q_SOC_GLOBAL_RESET), rd(gBar0, Q_SOC_RESET_CONTROL), rd(gBar0, Q_SOC_LF_TIMER0),
	    rd(gBar0, Q_SOC_CHIP_ID), rd(gBar0, Q_FW_INDICATOR), rd(gBar0, Q_PCIE_INTR_ENABLE), rd(gBar0, Q_PCIE_INTR_CAUSE));
}

static inline bool
chip_id_valid(uint32_t c)
{
	return c != 0u && c != 0xFFFFFFFFu;
}

static inline bool
fw_ind_done(uint32_t f)
{
	return f != 0xFFFFFFFFu && (f & (Q_FW_IND_INITIALIZED | Q_FW_IND_EVENT_PENDING)) != 0;
}

/*
 * Hold the SoC awake and poll every 10 ms until the chip has initialised, logging each change of chip ID /
 * FW_INDICATOR. ath10k reads SOC_CHIP_ID only after ath10k_pci_wait_for_target_init (10 ms polls, 3 s);
 * read too early, "the device doesn't init quickly enough" and the ID reads 0 (ath10k commit "reset chip
 * before reading chip_id in probe"). fw_init: stop when FW_INDICATOR reports initialized (after a reset);
 * otherwise stop 500 ms after the chip ID turns valid, or sooner if FW_INDICATOR reports first.
 * Returns the ms at which the chip ID was first valid (or the ms waited, if it never was).
 */
static uint32_t
q_wait_init(const char *tag, uint32_t max_ms, bool fw_init, uint32_t *chip, uint32_t *fwi)
{
	uint32_t c = 0xDEADBEEFu, f = 0xDEADBEEFu, rtc = 0, t = 0, valid_at = 0;
	bool valid = false;
	for (;;) {
		bool awake = q_wake(&rtc);
		uint32_t nc = rd(gBar0, Q_SOC_CHIP_ID), nf = rd(gBar0, Q_FW_INDICATOR);
		if (nc != c || nf != f || !awake) {
			W_LOG("%s %4u ms (%u ms after WLAN_EN): chip id %08x fw indicator %08x rtc_state %08x%s", tag, t,
			    ms_since_power_on(), nc, nf, rtc, awake ? "" : " NOT AWAKE");
			c = nc;
			f = nf;
		}
		if (!valid && chip_id_valid(c)) {
			valid = true;
			valid_at = t;
		}
		bool done = fw_init ? fw_ind_done(f) : (valid && (fw_ind_done(f) || t - valid_at >= 500u));
		if (done || t >= max_ms) {
			break;
		}
		IOSleep(10);
		t += 10;
	}
	*chip = rd(gBar0, Q_SOC_CHIP_ID);   /* still awake: final values */
	*fwi = rd(gBar0, Q_FW_INDICATOR);
	return valid ? valid_at : t;
}

/*
 * ath10k_pci_cold_reset: SOC_GLOBAL_RESET bit 0 resets the whole chip, its PCIe core included; no PCIe
 * access for 20 ms after setting or clearing it. qcacld-2.0 (hif_pci_device_reset): never write it without
 * WAKE_V, "the Target may scribble over Host memory" (bus mastering is off here anyway). Added here: before
 * each BAR access, check the link from the RC side and the endpoint's config space (BAR0, memory decoding),
 * and rewrite those if the reset cleared them.
 */
static bool
q_cold_reset(uint32_t bar_lo, bool bar64, char *why, size_t n)
{
	uint32_t rtc = 0;
	if (!q_wake(&rtc)) {
		snprintf(why, n, "SoC not awake (rtc_state %08x)", rtc);
		return false;
	}
	uint32_t v = rd(gBar0, Q_SOC_GLOBAL_RESET);
	W_LOG("cold reset: global_reset %08x, setting bit 0", v);
	wr(gBar0, Q_SOC_GLOBAL_RESET, v | 1u);
	IOSleep(20);
	for (int phase = 0; phase < 2; phase++) {
		const char *what = phase == 0 ? "in reset" : "released";
		bool up = wait_rc_link(200);
		uint32_t id = up ? rd(gConf, 0x0) : 0xFFFFFFFFu, bar = up ? rd(gConf, 0x10) : 0u, cmd = up ? rd(gConf, 0x04) : 0u;
		W_LOG("cold reset, %s: rc link %s (elbi %08x lnksta %08x ltssm %08x), ep id %08x bar0 %08x command %08x", what,
		    up ? "up" : "DOWN", rd(gDbi, ELBI_SYS_STTS), rd(gDbi, DBI_LINK_STATUS), rd(gParf, PARF_LTSSM), id, bar, cmd);
		if (!up || id != 0x003E168Cu) {
			snprintf(why, n, "%s: link %s, ep id %08x", what, up ? "up" : "down", id);
			return false;
		}
		if ((bar & ~0xFu) != (bar_lo & ~0xFu) || !(cmd & 2u)) {
			wr(gConf, 0x10, bar_lo);
			if (bar64) {
				wr(gConf, 0x14, 0u);
			}
			wr(gConf, 0x04, (cmd & ~0x7u) | 0x2u);
			W_LOG("cold reset, %s: config space was cleared; rewritten: bar0 %08x command %08x", what,
			    rd(gConf, 0x10), rd(gConf, 0x04));
		}
		if (phase == 0) {
			wr(gBar0, Q_SOC_GLOBAL_RESET, v & ~1u);
			IOSleep(20);
		}
	}
	return true;
}

static bool
step2a(void)
{
	char line[320];

	/* Root port (DBI = its own config space): bus 0 -> secondary 1, subordinate 1; memory window = the
	 * 7 MB "bars" range; memory decoding on. Bus mastering stays off on both sides. */
	wr(gDbi, 0x18, 0x00010100u);
	wr(gDbi, 0x20, (uint32_t)((W_BARS_PHYS + W_BARS_SIZE - 1) & 0xFFF00000u) | (uint32_t)((W_BARS_PHYS >> 16) & 0xFFF0u));
	wr(gDbi, 0x04, (rd(gDbi, 0x04) & ~0x4u) | 0x2u);

	/* Size BAR0 (64-bit memory BAR: BAR0 low, BAR1 high), then place it at the start of the window. */
	uint32_t orig = rd(gConf, 0x10);
	wr(gConf, 0x10, 0xFFFFFFFFu);
	uint32_t mask = rd(gConf, 0x10);
	wr(gConf, 0x10, orig);
	uint64_t size = (uint64_t)(~(mask & ~0xFu)) + 1u;
	size &= 0xFFFFFFFFu;
	W_LOG("BAR0: was %08x, sizing mask %08x -> %llu KB (%s)", orig, mask, size / 1024,
	    (mask & 0x6u) == 0x4u ? "64-bit memory" : "32-bit memory");
	if ((mask & 1u) || size == 0 || size > W_BARS_SIZE) {
		snprintf(line, sizeof(line), "STEP 2a FAIL: BAR0 unusable (mask %08x, size %llu)", mask, size);
		summary(line);
		return false;
	}
	/* A BAR is aligned to its own size: 2 MB here, so the first usable address in the window is 0xf8a00000
	 * (0xf8900000 is not 2 MB-aligned; the chip dropped bit 20 and put BAR0 at 0xf8800000). PCI bus address =
	 * CPU address (pci-msm: mem_offset 0; the window is passed to PCIe untranslated). */
	uint64_t addr = (W_BARS_PHYS + size - 1u) & ~(size - 1u);
	if (addr + size > W_BARS_PHYS + W_BARS_SIZE) {
		snprintf(line, sizeof(line), "STEP 2a FAIL: no %llu KB-aligned slot for BAR0 in the window", size / 1024);
		summary(line);
		return false;
	}
	wr(gConf, 0x10, (uint32_t)addr | (mask & 0xFu));
	if ((mask & 0x6u) == 0x4u) {
		wr(gConf, 0x14, 0u);
	}
	wr(gConf, 0x04, (rd(gConf, 0x04) & ~0x7u) | 0x2u);   /* memory space only */
	uint32_t got = rd(gConf, 0x10);
	W_LOG("BAR0 at 0x%08x (wanted 0x%08llx), ep command %08x, rp command %08x, rp window %08x", got, addr,
	    rd(gConf, 0x04), rd(gDbi, 0x04), rd(gDbi, 0x20));
	if ((got & ~0xFu) != (uint32_t)addr) {
		snprintf(line, sizeof(line), "STEP 2a FAIL: BAR0 reads back %08x, wrote %08llx", got, addr);
		summary(line);
		return false;
	}

	gBar0 = (volatile uint32_t *)ml_io_map((vm_offset_t)addr, (vm_size_t)size);
	if (gBar0 == NULL) {
		summary("STEP 2a FAIL: cannot map BAR0");
		return false;
	}

	gBar0Lo = (uint32_t)addr | (mask & 0xFu);
	gBar64 = (mask & 0x6u) == 0x4u;

	/* Wake the chip's SoC, then wait (no reset) until it has initialised itself. */
	uint32_t rtc = 0;
	if (!q_wake(&rtc)) {
		wr(gBar0, Q_SOC_WAKE, 0u);
		snprintf(line, sizeof(line), "STEP 2a FAIL: chip SoC did not wake in 30 ms (rtc_state %08x)", rtc);
		summary(line);
		return false;
	}
	W_LOG("SoC awake %u ms after WLAN_EN", ms_since_power_on());
	q_snapshot("BAR0 regs");
	uint32_t chip = 0, fwi = 0;
	uint32_t t_valid = q_wait_init("wait", 5000, false, &chip, &fwi);
	const char *how = "no reset";
	if (chip == 0xFFFFFFFFu) {
		snprintf(line, sizeof(line), "STEP 2a FAIL: BAR0 reads all ones after the wait (link %s): no reset tried",
		    rc_link_up() ? "up" : "down");
		summary(line);
		return false;
	}
	if (!chip_id_valid(chip)) {
		/* Fallback, the ath10k path: cold reset, then wait for the boot ROM's "initialized" (3 s). */
		q_snapshot("BAR0 regs after 5 s, before cold reset");
		char why[120];
		if (!q_cold_reset((uint32_t)addr | (mask & 0xFu), (mask & 0x6u) == 0x4u, why, sizeof(why))) {
			snprintf(line, sizeof(line), "STEP 2a FAIL: chip id %08x fw indicator %08x after 5 s; cold reset: %s",
			    chip, fwi, why);
			summary(line);
			return false;   /* chip may be held in reset or off the bus: no further BAR access */
		}
		t_valid = q_wait_init("after cold reset", 3000, true, &chip, &fwi);
		q_snapshot("BAR0 regs after cold reset");
		how = "after cold reset";
	}
	wr(gBar0, Q_SOC_WAKE, 0u);

	uint32_t rev = (chip >> 8) & 0xFu;
	if (!chip_id_valid(chip)) {
		snprintf(line, sizeof(line), "STEP 2a FAIL: chip id %08x fw indicator %08x after 5 s wait + cold reset + 3 s wait%s",
		    chip, fwi, fwi == 0xFFFFFFFFu ? " (reads all ones)" : (fwi & Q_FW_IND_EVENT_PENDING) ? " (target crashed)" : "");
	} else {
		snprintf(line, sizeof(line), "%s: chip id %08x (rev %u, hw3.2 = %u), fw indicator %08x, valid at %u ms (%s)",
		    rev == Q_HW32_CHIP_REV ? "STEP 2a PASS" : "STEP 2a: unexpected chip rev", chip, rev, Q_HW32_CHIP_REV, fwi,
		    t_valid, how);
	}
	summary(line);
	return chip_id_valid(chip) && rev == Q_HW32_CHIP_REV;
}

/* steps 2b to 8 (wifi=4 to 11): copy engines and BMI, firmware boot, WMI/HTT services (OP2Ath10k.cpp) */
static bool
cold_reset_for_2b(char *why, size_t n)
{
	return q_cold_reset(gBar0Lo, gBar64, why, n);
}

static void
step2b(void)
{
	char line[320];
	struct op2_ath10k_bus bus = { gBar0, gConf, gDbi, gPowerOnAbs, cold_reset_for_2b, gMode >= 5, gMode >= 6,
		gMode >= 7, gMode >= 8, gMode >= 9, gMode >= 10, gMode >= 11, 0, gMode >= 12 };
	(void)op2_ath10k_step2b(&bus, line, sizeof(line));
	summary(line);
}

/* ---------------------------------------------------------------------------------------------------- */

/* bt_vreg (PM8994 GPIO 9, the chip's vdd-wlan / vdd-bt-pa) high; NULL, or why not. Shared by Wi-Fi and Bluetooth. */
static const char *
bt_vreg_on(bool bt)
{
	uint8_t mode = 0, en = 0, out = 0;
	if (!op2_spmi_read(PM_SID, PMGPIO9_BASE + PM_MODE_CTL, &mode, 1) ||
	    !op2_spmi_read(PM_SID, PMGPIO9_BASE + PM_EN_CTL, &en, 1) ||
	    !op2_spmi_read(PM_SID, PMGPIO9_BASE + PM_DIG_OUT_CTL, &out, 1)) {
		return "FAIL: SPMI read of PM8994 GPIO 9 (bt_vreg); is OP2Battery running?";
	}
	if (mode != 0x11 || !(en & 0x80)) {
		/* qpnp-pin: MODE_CTL = mode 1 (digital out) << 4 | source 0 (constant) << 1 | invert 1 -> high */
		uint8_t m = 0x11, e = 0x80, o = (uint8_t)((out & 0x30) | ((out & 3) ? (out & 3) : 2));
		bool ok = op2_spmi_write(PM_SID, PMGPIO9_BASE + PM_DIG_OUT_CTL, &o, 1) &&
		    op2_spmi_write(PM_SID, PMGPIO9_BASE + PM_MODE_CTL, &m, 1) &&
		    op2_spmi_write(PM_SID, PMGPIO9_BASE + PM_EN_CTL, &e, 1);
		if (bt) {
			B_LOG("bt_vreg (PM GPIO 9): was mode %02x en %02x out %02x, set high: %s", mode, en, out, ok ? "ok" : "FAILED");
		} else {
			W_LOG("bt_vreg (PM GPIO 9): was mode %02x en %02x out %02x, set high: %s", mode, en, out, ok ? "ok" : "FAILED");
		}
		if (!ok) {
			return "FAIL: SPMI write to PM8994 GPIO 9 (bt_vreg)";
		}
		IOSleep(5);   /* regulator-fixed startup-delay-us = 4000 */
	} else if (bt) {
		B_LOG("%s", "bt_vreg (PM GPIO 9) already high");
	} else {
		W_LOG("%s", "bt_vreg (PM GPIO 9) already high");
	}
	return NULL;
}

/* ---------------------------------------------------------------------------------------------------- */
/* Bluetooth (boot-arg bt): see the top of the file                                                      */

static char gBtSummary[240] = "not run";

static void
bt_summary(const char *s)
{
	strlcpy(gBtSummary, s, sizeof(gBtSummary));
	B_LOG("RESULT: %s", gBtSummary);
}

static void
bt_pin(const char *name, uint32_t p)
{
	uint32_t cfg = rd(gTlmm, TLMM_CFG(p)), io = rd(gTlmm, TLMM_IO(p));
	B_LOG("pin %u %-4s cfg=0x%03x (func %u drv %u pull %u oe %u) in=%u out=%u", p, name, cfg,
	    (cfg >> 2) & 0xf, (cfg >> 6) & 7, cfg & 3, (cfg >> 9) & 1, io & 1, (io >> 1) & 1);
}

static void
bt_pmic(const char *name, uint16_t base)
{
	uint8_t st = 0, mode = 0, vin = 0, out = 0, en = 0;
	bool ok = op2_spmi_read(PM_SID, (uint16_t)(base + PM_STATUS1), &st, 1) &&
	    op2_spmi_read(PM_SID, (uint16_t)(base + PM_MODE_CTL), &mode, 1) &&
	    op2_spmi_read(PM_SID, (uint16_t)(base + PM_DIG_VIN_CTL), &vin, 1) &&
	    op2_spmi_read(PM_SID, (uint16_t)(base + PM_DIG_OUT_CTL), &out, 1) &&
	    op2_spmi_read(PM_SID, (uint16_t)(base + PM_EN_CTL), &en, 1);
	if (!ok) {
		B_LOG("pmic %-8s 0x%04x: SPMI read failed", name, base);
		return;
	}
	B_LOG("pmic %-8s 0x%04x: status1 %02x mode %02x vin %02x out %02x en %02x", name, base, st, mode, vin, out, en);
}

static bool
bt_uart_clocked(void)
{
	return !(rd(gGcc, GCC_BLSP2_AHB_CBCR) & CBCR_OFF) && !(rd(gGcc, GCC_BLSP2_UART2_APPS_CBCR) & CBCR_OFF);
}

static void
bt_dump(const char *when)
{
	bt_pin("tx", PIN_BT_TX);
	bt_pin("rx", PIN_BT_RX);
	bt_pin("cts", PIN_BT_CTS);
	bt_pin("rfr", PIN_BT_RFR);
	bt_pmic("bt_reset", PMGPIO19_BASE);
	bt_pmic("bt_vreg", PMGPIO9_BASE);
	bt_pmic("L30", PMLDO(30));
	uint32_t r = GCC_BLSP2_UART2_CMD_RCGR;
	B_LOG("%s: clocks: blsp2 ahb %08x (vote %s), uart2 apps %08x, rcg cmd %08x cfg %08x m %08x n %08x d %08x", when,
	    rd(gGcc, GCC_BLSP2_AHB_CBCR), (rd(gGcc, GCC_APCS_BRANCH_VOTE) & (1u << 15)) ? "on" : "off",
	    rd(gGcc, GCC_BLSP2_UART2_APPS_CBCR), rd(gGcc, r), rd(gGcc, r + 4), rd(gGcc, r + 8), rd(gGcc, r + 12),
	    rd(gGcc, r + 16));
	if (!bt_uart_clocked()) {
		B_LOG("%s: the UART's clocks are off: its registers not read", when);
		return;
	}
	volatile uint32_t *u = (volatile uint32_t *)ml_io_map(W_BTUART_PHYS, 0x1000);
	if (u == NULL) {
		B_LOG("%s: ml_io_map of the UART failed", when);
		return;
	}
	B_LOG("%s: uart: mr1 %08x mr2 %08x sr %08x isr %08x txfs %08x rxfs %08x", when, rd(u, UDM_MR1), rd(u, UDM_MR2),
	    rd(u, UDM_SR), rd(u, UDM_ISR), rd(u, UDM_TXFS), rd(u, UDM_RXFS));
}

/* PM8994 GPIO 19: Bluetooth reset (low = held in reset). Digital output, vin 2 as the OnePlus kernel sets it. */
static bool
bt_reset_line(bool run)
{
	uint8_t vin = 0, out = 0;
	if (!op2_spmi_read(PM_SID, PMGPIO19_BASE + PM_DIG_VIN_CTL, &vin, 1) ||
	    !op2_spmi_read(PM_SID, PMGPIO19_BASE + PM_DIG_OUT_CTL, &out, 1)) {
		return false;
	}
	uint8_t v = 2, o = (uint8_t)((out & 0x30) | ((out & 3) ? (out & 3) : 2)), m = run ? 0x11 : 0x10, e = 0x80;
	return (vin == 2 || op2_spmi_write(PM_SID, PMGPIO19_BASE + PM_DIG_VIN_CTL, &v, 1)) &&
	       (o == out || op2_spmi_write(PM_SID, PMGPIO19_BASE + PM_DIG_OUT_CTL, &o, 1)) &&
	       op2_spmi_write(PM_SID, PMGPIO19_BASE + PM_MODE_CTL, &m, 1) &&
	       op2_spmi_write(PM_SID, PMGPIO19_BASE + PM_EN_CTL, &e, 1);
}

/* a pin as a GPIO input; pull: 1 = down, 3 = up */
static void
pin_in(uint32_t p, uint32_t pull)
{
	wr(gTlmm, TLMM_CFG(p), (2u << 6) | (pull & 3));
}

/* watch the chip's TX (our RX, pin 46) and RTS (our CTS, pin 47) for ms milliseconds; true: either moved */
static bool
bt_watch(const char *what, uint32_t ms)
{
	uint32_t rx0 = rd(gTlmm, TLMM_IO(PIN_BT_RX)) & 1, cts0 = rd(gTlmm, TLMM_IO(PIN_BT_CTS)) & 1;
	uint32_t rx = rx0, cts = cts0, rx_changes = 0;
	int rx_hi = rx0 ? 0 : -1, cts_lo = cts0 ? -1 : 0;
	for (uint32_t t = 0; t < ms; t++) {
		uint32_t r = rd(gTlmm, TLMM_IO(PIN_BT_RX)) & 1, c = rd(gTlmm, TLMM_IO(PIN_BT_CTS)) & 1;
		if (r != rx) {
			rx_changes++;
			rx = r;
		}
		if (rx_hi < 0 && r) {
			rx_hi = (int)t;
		}
		if (cts_lo < 0 && !c) {
			cts_lo = (int)t;
		}
		cts = c;
		IODelay(1000);
	}
	B_LOG("%s: %u ms: rx %u->%u (high at %d ms, %u changes), cts %u->%u (low at %d ms)", what, ms, rx0, rx, rx_hi,
	    rx_changes, cts0, cts, cts_lo);
	return rx_hi >= 0 || cts_lo >= 0;
}

/* reset low 50 ms, then high, then watch 5 s */
static bool
bt_cycle(const char *what)
{
	if (!bt_reset_line(false)) {
		B_LOG("%s: SPMI access to PM8994 GPIO 19 failed", what);
		return false;
	}
	IOSleep(50);
	uint32_t rx = rd(gTlmm, TLMM_IO(PIN_BT_RX)) & 1, cts = rd(gTlmm, TLMM_IO(PIN_BT_CTS)) & 1;
	B_LOG("%s: in reset: rx %u cts %u (expected: rx 0, cts 1, from the pulls)", what, rx, cts);
	if (!bt_reset_line(true)) {
		B_LOG("%s: SPMI write to PM8994 GPIO 19 failed", what);
		return false;
	}
	return bt_watch(what, 5000);
}

static void
bt_result(bool alive, const char *when)
{
	char line[240];
	(void)bt_reset_line(false);   /* back in reset until the next step needs it */
	if (alive) {
		snprintf(line, sizeof(line), "BT STEP 1 PASS: the chip came out of reset (%s); held in reset again", when);
	} else {
		snprintf(line, sizeof(line), "BT STEP 1 FAIL: no sign of the chip on its UART lines in 5 s after reset (%s)", when);
	}
	bt_summary(line);
}

static void
bt_step1(void)
{
	/* 1. Supplies (bt_power.c: vdd-io S4 1.8 V always on, vdd-xtal L30 1.8 V, vdd-pa bt_vreg). */
	if (!ldo_on(30, 1800000, 0)) {
		bt_summary("FAIL: RPM did not ack L30 (vdd-bt-xtal)");
		return;
	}
	const char *why = bt_vreg_on(true);
	if (why != NULL) {
		bt_summary(why);
		return;
	}
	/* 2. Our side of the UART as plain pins: TX idle high, RFR low (the chip may send); RX pulled down and CTS pulled
	 *    up, so the chip driving its TX high (idle) and its RTS low (ready) shows as a change on each. */
	pin_out(PIN_BT_TX, 1, true);
	pin_out(PIN_BT_RFR, 1, false);
	pin_in(PIN_BT_RX, 1);
	pin_in(PIN_BT_CTS, 3);
	IOSleep(20);
	/* 3. As found (2026-11-27: the reset line was already high and RX read 1: the chip was running), then a reset. */
	uint8_t m = 0;
	(void)op2_spmi_read(PM_SID, PMGPIO19_BASE + PM_MODE_CTL, &m, 1);
	B_LOG("as found: reset line %s (mode %02x)", m == 0x11 ? "high (running)" : m == 0x10 ? "low (in reset)" : "?", m);
	bool found = bt_watch("as found", 200);
	bool alive = bt_cycle("reset, before Wi-Fi");
	bt_dump("after");
	bt_result(alive, found ? "before Wi-Fi; it was running as found" : "before Wi-Fi; silent as found too");
}

/* ---------------------------------------------------------------------------------------------------- */
/* Bluetooth step 2 (bt=3): the UART and the first HCI commands                                          */

static volatile uint32_t *gBtUart;

static bool
wait_bits(volatile uint32_t *b, uint32_t off, uint32_t mask, bool set, uint32_t us)
{
	for (uint32_t t = 0; t < us; t += 10) {
		if (((rd(b, off) & mask) != 0) == set) {
			return true;
		}
		IODelay(10);
	}
	return ((rd(b, off) & mask) != 0) == set;
}

/* BLSP2 AHB (voted) and UART2 core clock at 7.3728 MHz: GPLL0 (600 MHz, src 1) * 192 / 15625, dual-edge M/N */
static bool
bt_uart_clocks(char *why, size_t n)
{
	wr(gGcc, GCC_APCS_BRANCH_VOTE, rd(gGcc, GCC_APCS_BRANCH_VOTE) | (1u << 15));
	if (!wait_bits(gGcc, GCC_BLSP2_AHB_CBCR, CBCR_OFF, false, 1000)) {
		snprintf(why, n, "BLSP2 AHB clock stays off (cbcr %08x)", rd(gGcc, GCC_BLSP2_AHB_CBCR));
		return false;
	}
	uint32_t r = GCC_BLSP2_UART2_CMD_RCGR;
	wr(gGcc, r, rd(gGcc, r) | 2u);                  /* CMD: root enable (the update needs a running root) */
	if (!wait_bits(gGcc, r, 1u << 31, false, 1000)) {
		snprintf(why, n, "UART2 clock root stays off (cmd %08x cfg %08x): GPLL0?", rd(gGcc, r), rd(gGcc, r + 4));
		return false;
	}
	wr(gGcc, r + 8, 192u);                          /* M */
	wr(gGcc, r + 12, ~(15625u - 192u) & 0xffffu);   /* N: ~(n - m) */
	wr(gGcc, r + 16, ~15625u & 0xffffu);            /* D: ~n */
	wr(gGcc, r + 4, 1u | (1u << 8) | (2u << 12));   /* CFG: pre-divider 1, source GPLL0, dual edge */
	wr(gGcc, r, rd(gGcc, r) | 1u);                  /* CMD: update */
	if (!wait_bits(gGcc, r, 1u, false, 1000)) {
		snprintf(why, n, "UART2 clock: the RCG update never finished (cmd %08x cfg %08x)", rd(gGcc, r), rd(gGcc, r + 4));
		return false;
	}
	wr(gGcc, GCC_BLSP2_UART2_APPS_CBCR, rd(gGcc, GCC_BLSP2_UART2_APPS_CBCR) | 1u);
	if (!wait_bits(gGcc, GCC_BLSP2_UART2_APPS_CBCR, CBCR_OFF, false, 1000)) {
		snprintf(why, n, "UART2 core clock stays off (cbcr %08x, rcg cmd %08x cfg %08x)",
		    rd(gGcc, GCC_BLSP2_UART2_APPS_CBCR), rd(gGcc, r), rd(gGcc, r + 4));
		return false;
	}
	B_LOG("2: clocks on: blsp2 ahb %08x, uart2 apps %08x, rcg cmd %08x cfg %08x m %08x n %08x d %08x (7.3728 MHz)",
	    rd(gGcc, GCC_BLSP2_AHB_CBCR), rd(gGcc, GCC_BLSP2_UART2_APPS_CBCR), rd(gGcc, r), rd(gGcc, r + 4),
	    rd(gGcc, r + 8), rd(gGcc, r + 12), rd(gGcc, r + 16));
	return true;
}

/* UART_DM v1.4, FIFO mode, polled: 115200 8N1, no automatic flow control, RFR asserted */
static void
bt_uart_init(void)
{
	volatile uint32_t *u = gBtUart;
	wr(u, UDM_CR, UDM_CR_RX_DIS | UDM_CR_TX_DIS);
	wr(u, UDM_CR, UDM_CMD_RESET_RX);
	wr(u, UDM_CR, UDM_CMD_RESET_TX);
	wr(u, UDM_CR, UDM_CMD_RESET_ERR);
	wr(u, UDM_CR, UDM_CMD_RESET_BRK);
	wr(u, UDM_CR, UDM_CMD_RESET_CTS);
	wr(u, UDM_CR, UDM_CMD_RESET_STALE);
	wr(u, UDM_DMEN, 0);
	wr(u, UDM_IMR, 0);
	wr(u, UDM_MR1, 0);
	wr(u, UDM_MR2, 0x34u);   /* 8 bits, 1 stop bit, no parity */
	wr(u, UDM_CSR, 0xCCu);   /* 7.3728 MHz / 16 / 4 = 115200 */
	wr(u, UDM_CR, UDM_CMD_RFR_LOW);
	wr(u, UDM_DMRX, 0xFFFFFFu);
	wr(u, UDM_CR, UDM_CMD_STALE_ENABLE);
	wr(u, UDM_CR, UDM_CR_RX_EN | UDM_CR_TX_EN);
}

static bool
bt_uart_send(const uint8_t *b, uint32_t n)
{
	volatile uint32_t *u = gBtUart;
	for (uint32_t t = 0; t < 20000 && !(rd(u, UDM_SR) & UDM_SR_TXEMT); t += 10) {
		if (rd(u, UDM_ISR) & UDM_ISR_TXREADY) {
			break;
		}
		IODelay(10);
	}
	wr(u, UDM_CR, UDM_CMD_CLEAR_TX_READY);
	wr(u, UDM_NCF_TX, n);
	(void)rd(u, UDM_NCF_TX);
	for (uint32_t i = 0; i < n; i += 4) {
		if (!wait_bits(u, UDM_SR, UDM_SR_TXRDY, true, 20000)) {
			return false;
		}
		uint32_t w = 0;
		for (uint32_t k = 0; k < 4 && i + k < n; k++) {
			w |= (uint32_t)b[i + k] << (8 * k);
		}
		wr(u, UDM_TF, w);
	}
	return wait_bits(u, UDM_SR, UDM_SR_TXEMT, true, 20000);   /* gone out of the shifter (at 115200: 87 us a byte) */
}

/* whatever arrives in ms milliseconds (full FIFO words, then a partial word pushed out by a forced stale event) */
static uint32_t
bt_uart_recv(uint8_t *out, uint32_t max, uint32_t ms)
{
	volatile uint32_t *u = gBtUart;
	uint32_t got = 0, quiet = 0;
	for (uint32_t t = 0; t < ms * 10; t++) {   /* 100 us steps */
		if (rd(u, UDM_SR) & UDM_SR_RXRDY) {
			uint32_t w = rd(u, UDM_RF);
			for (uint32_t k = 0; k < 4 && got < max; k++) {
				out[got++] = (uint8_t)(w >> (8 * k));
			}
			quiet = 0;
			continue;
		}
		uint32_t pend = (rd(u, UDM_RXFS) >> 7) & 7u;   /* characters in the packing buffer (less than a word) */
		if (pend != 0 && ++quiet >= 20) {           /* 2 ms with no new word: the rest of a message */
			wr(u, UDM_CR, UDM_CMD_FORCE_STALE);
			IODelay(50);
			uint32_t w = rd(u, UDM_RF);
			for (uint32_t k = 0; k < pend && k < 4 && got < max; k++) {
				out[got++] = (uint8_t)(w >> (8 * k));
			}
			wr(u, UDM_CR, UDM_CMD_RESET_STALE);
			wr(u, UDM_DMRX, 0xFFFFFFu);
			wr(u, UDM_CR, UDM_CMD_STALE_ENABLE);
			quiet = 0;
		}
		IODelay(100);
	}
	return got;
}

static void
hex_line(char *out, size_t n, const uint8_t *b, uint32_t len)
{
	size_t o = 0;
	out[0] = '\0';
	for (uint32_t i = 0; i < len && o + 4 < n; i++) {
		o += (size_t)snprintf(out + o, n - o, "%s%02x", i ? " " : "", b[i]);
	}
	if (len == 0) {
		snprintf(out, n, "nothing");
	}
}

/* send a command, log the answer; the number of bytes that came back */
static uint32_t
bt_hci(const char *what, const uint8_t *cmd, uint32_t n, uint8_t *ans, uint32_t max)
{
	char h[200], a[220];
	hex_line(h, sizeof(h), cmd, n);
	if (!bt_uart_send(cmd, n)) {
		B_LOG("2: %s: sending %s: the UART did not take it (sr %08x isr %08x txfs %08x)", what, h, rd(gBtUart, UDM_SR),
		    rd(gBtUart, UDM_ISR), rd(gBtUart, UDM_TXFS));
		return 0;
	}
	uint32_t got = bt_uart_recv(ans, max, 500);
	hex_line(a, sizeof(a), ans, got);
	B_LOG("2: %s: sent %s; %u bytes back: %s (sr %08x, cts %s)", what, h, got, a, rd(gBtUart, UDM_SR),
	    (rd(gBtUart, UDM_ISR) & UDM_ISR_CTS) ? "1" : "0");
	return got;
}

static void
bt_step2(void)
{
	char why[200], a[220];
	uint8_t ans[96];
	if (!ldo_on(30, 1800000, 0)) {
		bt_summary("BT STEP 2 FAIL: RPM did not ack L30 (vdd-bt-xtal)");
		return;
	}
	const char *vr = bt_vreg_on(true);
	if (vr != NULL) {
		bt_summary(vr);
		return;
	}
	if (!bt_reset_line(false)) {   /* the chip held in reset while the UART is set up */
		bt_summary("BT STEP 2 FAIL: SPMI access to PM8994 GPIO 19 (Bluetooth reset)");
		return;
	}
	if (!bt_uart_clocks(why, sizeof(why))) {
		snprintf(a, sizeof(a), "BT STEP 2 FAIL: %s", why);
		bt_summary(a);
		return;
	}
	if (gBtUart == NULL) {
		gBtUart = (volatile uint32_t *)ml_io_map(W_BTUART_PHYS, 0x1000);
	}
	if (gBtUart == NULL) {
		bt_summary("BT STEP 2 FAIL: ml_io_map of the UART");
		return;
	}
	for (uint32_t p = PIN_BT_TX; p <= PIN_BT_RFR; p++) {   /* blsp2_uart2_active: function 2, 16 mA, no pull */
		wr(gTlmm, TLMM_CFG(p), (2u << 2) | (7u << 6));
	}
	bt_uart_init();
	IOSleep(50);
	B_LOG("2: UART up (115200 8N1, polled): sr %08x isr %08x mr1 %08x mr2 %08x rxfs %08x; chip in reset, cts %s",
	    rd(gBtUart, UDM_SR), rd(gBtUart, UDM_ISR), rd(gBtUart, UDM_MR1), rd(gBtUart, UDM_MR2), rd(gBtUart, UDM_RXFS),
	    (rd(gBtUart, UDM_ISR) & UDM_ISR_CTS) ? "1" : "0");
	uint32_t junk = bt_uart_recv(ans, sizeof(ans), 20);   /* line noise from the pin change */
	if (!bt_reset_line(true)) {
		bt_summary("BT STEP 2 FAIL: SPMI write to PM8994 GPIO 19 (Bluetooth reset)");
		return;
	}
	uint32_t got = bt_uart_recv(ans, sizeof(ans), 500);
	hex_line(a, sizeof(a), ans, got);
	B_LOG("2: out of reset (%u bytes of noise before): 500 ms: %u bytes by itself: %s (sr %08x, cts %s)", junk, got, a,
	    rd(gBtUart, UDM_SR), (rd(gBtUart, UDM_ISR) & UDM_ISR_CTS) ? "1" : "0");
	static const uint8_t getver[] = { 0x01, 0x00, 0xfc, 0x01, 0x19 };   /* Qualcomm EDL_PATCH_GETVER */
	static const uint8_t reset[] = { 0x01, 0x03, 0x0c, 0x00 };          /* HCI_Reset */
	uint32_t v = bt_hci("get version", getver, sizeof(getver), ans, sizeof(ans));
	uint32_t r = bt_hci("HCI reset", reset, sizeof(reset), ans, sizeof(ans));
	bool reset_ok = false;
	for (uint32_t i = 0; i + 7 <= r; i++) {
		if (ans[i] == 0x04 && ans[i + 1] == 0x0e && ans[i + 3] == 0x01 && ans[i + 4] == 0x03 && ans[i + 5] == 0x0c) {
			reset_ok = ans[i + 6] == 0x00;
			snprintf(a, sizeof(a), "BT STEP 2 PASS: the chip answers over the UART: HCI Reset complete (status %02x)%s",
			    ans[i + 6], v ? ", get version answered too" : "");
			break;
		}
	}
	if (!reset_ok) {
		if (v + r + got > 0) {
			snprintf(a, sizeof(a), "BT STEP 2: the chip sends something (%u, %u and %u bytes) but no HCI Reset complete: "
			    "see the lines above", got, v, r);
		} else {
			snprintf(a, sizeof(a), "BT STEP 2 FAIL: no byte from the chip at 115200 (by itself, to get version, to HCI "
			    "Reset); cts %s", (rd(gBtUart, UDM_ISR) & UDM_ISR_CTS) ? "1" : "0");
		}
	}
	(void)bt_reset_line(false);   /* back in reset until the next step needs it */
	wr(gBtUart, UDM_CR, UDM_CR_RX_DIS | UDM_CR_TX_DIS);
	bt_summary(a);
}

static void
bringup(void)
{
	char line[320];

	/* 1. Hold the chip's PCIe interface in reset while everything is powered up. */
	pin_out(PIN_PERST, 0, false);
	IODelay(1000);

	/* 2. Chip power (cnss.c): vdd-wlan (bt_vreg = PM8994 GPIO 9), vdd-wlan-io (S4 1.8 V, always on),
	 *    vdd-wlan-xtal (L30 1.8 V); 2 ms; bootstrap high, 10 ms; WLAN_EN high, 10 ms. */
	if (!ldo_on(30, 1800000, 0)) {
		summary("FAIL: RPM did not ack L30 (vdd-wlan-xtal)");
		return;
	}
	const char *why = bt_vreg_on(false);
	if (why != NULL) {
		summary(why);
		return;
	}
	IOSleep(2);
	pin_out(PIN_BOOTSTRAP, 7, true);
	IOSleep(10);
	pin_out(PIN_WLAN_EN, 7, true);
	clock_get_uptime(&gPowerOnAbs);
	IOSleep(10);
	W_LOG("%s", "chip power on: bt_vreg, L30, bootstrap, WLAN_EN");

	/* 3. RC1 supplies (msm_pcie_vreg_info): vreg-1.8 = L12 1.8 V 14 mA, vreg-0.9 = L28 1.0 V 40 mA. */
	if (!ldo_on(12, 1800000, 14) || !ldo_on(28, 1000000, 40)) {
		summary("FAIL: RPM did not ack the PCIe PHY supplies L12/L28");
		return;
	}

	/* 4. Power domain, then clocks (msm_pcie_clk_init order). */
	wr(gGcc, GCC_PCIE_1_GDSCR, rd(gGcc, GCC_PCIE_1_GDSCR) & ~GDSC_SW_COLLAPSE);
	bool gdsc = false;
	for (int i = 0; i < 100 && !gdsc; i++) {
		gdsc = (rd(gGcc, GCC_PCIE_1_GDSCR) & GDSC_PWR_ON) != 0;
		if (!gdsc) {
			IODelay(10);
		}
	}
	if (!gdsc) {
		snprintf(line, sizeof(line), "FAIL: GDSC pcie_1 did not power on (gdscr %08x)", rd(gGcc, GCC_PCIE_1_GDSCR));
		summary(line);
		return;
	}
	if (!op2_rpm_set_enable(RPM_CLKA, RPM_LN_BB_ID, true)) {
		summary("FAIL: RPM did not ack ln_bb_clk (PCIe reference clock)");
		return;
	}
	/* aux 1.011 MHz = XO 19.2 MHz * 1/19 (M/N, dual-edge), src 0 div 1: F(1011000, gcc_xo, 1, 1, 19) */
	wr(gGcc, GCC_PCIE_1_AUX_CMD_RCGR + 0x8, 1u);
	wr(gGcc, GCC_PCIE_1_AUX_CMD_RCGR + 0xC, ~(19u - 1u));
	wr(gGcc, GCC_PCIE_1_AUX_CMD_RCGR + 0x10, ~19u);
	wr(gGcc, GCC_PCIE_1_AUX_CMD_RCGR + 0x4, (2u << 12) | (0u << 8) | 1u);
	if (!rcg_update(GCC_PCIE_1_AUX_CMD_RCGR) ||
	    !branch_on(GCC_PCIE_1_AUX_CBCR, "aux", true) ||
	    !branch_on(GCC_PCIE_1_CFG_AHB_CBCR, "cfg_ahb", true) ||
	    !branch_on(GCC_PCIE_1_MSTR_AXI_CBCR, "mstr_axi", true) ||
	    !branch_on(GCC_PCIE_1_SLV_AXI_CBCR, "slv_axi", true)) {
		dump_clocks("clock failure");
		summary("FAIL: a PCIe controller clock did not start");
		return;
	}
	wr(gGcc, GCC_PCIE_1_PHY_LDO_EN, rd(gGcc, GCC_PCIE_1_PHY_LDO_EN) | 1u);
	wr(gGcc, GCC_PCIE_PHY_1_BCR, 1u);   /* pulse the PHY reset */
	IODelay(1000);
	wr(gGcc, GCC_PCIE_PHY_1_BCR, 0u);
	IODelay(1000);
	dump_clocks("clocks on");

	/* 5. TZ secure config for this controller (qcom,scm-dev-id = <12>); logged, not fatal (as in Linux). */
	uint32_t scm_ret = 0xdead;
	int32_t rc = scm_restore_sec_cfg(12, &scm_ret);
	W_LOG("SCM restore_sec_cfg(12): rc %d ret %u", rc, scm_ret);

	if (!pcie_clocked()) {
		summary("FAIL: controller still not clocked; PCIe registers not touched");
		return;
	}

	/* 6. Controller setup and PHY (msm_pcie_enable). */
	wr(gParf, PARF_PHY_CTRL, rd(gParf, PARF_PHY_CTRL) & ~1u);
	wr(gParf, PARF_DBI_BASE_ADDR, 0);
	wr(gParf, PARF_SYS_CTRL, 0x365E);
	for (size_t i = 0; i < sizeof(kPhyInit) / sizeof(kPhyInit[0]); i++) {
		wr(gPhy, kPhyInit[i][0], kPhyInit[i][1]);
	}
	IODelay(1000);
	/* pipe clock 125 MHz from the PHY: src 2 (pcie_pipe), div 1 */
	wr(gGcc, GCC_PCIE_PHY_1_PHY_BCR, 0u);
	wr(gGcc, GCC_PCIE_1_PIPE_CMD_RCGR + 0x4, (2u << 8) | 1u);
	(void)rcg_update(GCC_PCIE_1_PIPE_CMD_RCGR);
	(void)branch_on(GCC_PCIE_1_PIPE_CBCR, "pipe", false);   /* halt_check DELAY in Linux */
	bool phy = false;
	for (int i = 0; i < 10 && !phy; i++) {
		phy = !(rd(gPhy, PHY_PCS_STATUS) & (1u << 6));
		if (!phy) {
			IODelay(1000);
		}
	}
	dump_pcie("after PHY init");
	if (!phy) {
		snprintf(line, sizeof(line), "FAIL: PCIe PHY not ready (pcs_status %08x)", rd(gPhy, PHY_PCS_STATUS));
		summary(line);
		return;
	}

	/* 7. Let the chip out of reset and train the link. */
	IOSleep(10);   /* qcom,ep-latency = <10> */
	pin_out(PIN_PERST, 0, true);
	IODelay(1000);
	wr(gDbi, DBI_DEVCTL_STATUS, (rd(gDbi, DBI_DEVCTL_STATUS) & ~0x7000u) | (5u << 12));   /* max read 4 KB */
	wr(gParf, PARF_LTSSM, rd(gParf, PARF_LTSSM) | (1u << 8));
	bool up = false;
	for (int i = 0; i < 20 && !up; i++) {
		IODelay(5000);
		up = (rd(gDbi, ELBI_SYS_STTS) & XMLH_LINK_UP) && (rd(gDbi, DBI_LINK_STATUS) & (1u << 29)) &&
		    rd(gDbi, 0x000) != 0xFFFFFFFFu;
	}
	dump_pcie("after link training");
	if (!up) {
		pin_out(PIN_PERST, 0, false);
		snprintf(line, sizeof(line), "FAIL: link did not come up (elbi %08x ltssm %08x link %08x)",
		    rd(gDbi, ELBI_SYS_STTS), rd(gParf, PARF_LTSSM), rd(gDbi, DBI_LINK_STATUS));
		summary(line);
		return;
	}

	/* 8. iATU region 0: the 4 KB config window -> bus 1, device 0, function 0 (type CFG0). */
	wr(gDbi, IATU_VIEWPORT, 0);
	wr(gDbi, IATU_CTRL2, 0);
	wr(gDbi, IATU_CTRL1, 0x4);
	wr(gDbi, IATU_LBAR, (uint32_t)W_CONF_PHYS);
	wr(gDbi, IATU_UBAR, 0);
	wr(gDbi, IATU_LAR, (uint32_t)W_CONF_PHYS + 0xFFF);
	wr(gDbi, IATU_LTAR, 1u << 24);
	wr(gDbi, IATU_UTAR, 0);
	wr(gDbi, IATU_CTRL2, 1u << 31);
	IODelay(10);
	uint32_t id = rd(gConf, 0x0), cls = rd(gConf, 0x8), bar0 = rd(gConf, 0x10);
	snprintf(line, sizeof(line), "%s: endpoint id %04x:%04x class/rev %08x bar0 %08x (QCA6174 is 168c:003e)",
	    (id & 0xFFFF) == 0x168C ? "PASS" : "LINK UP, unexpected id", id & 0xFFFF, id >> 16, cls, bar0);
	summary(line);
	if (id == 0x003E168Cu && gMode >= 3 && step2a() && gMode >= 4) {
		step2b();
	}
}

static void __attribute__((noreturn))
wifi_thread(__unused void *param, __unused wait_result_t wr_)
{
	IOSleep(120000);
	gGcc = (volatile uint32_t *)ml_io_map(W_GCC_PHYS, 0x2000);
	gTlmm = (volatile uint32_t *)ml_io_map(W_TLMM_PHYS, 0x4000);
	gParf = (volatile uint32_t *)ml_io_map(W_PARF_PHYS, 0x2000);
	gPhy = (volatile uint32_t *)ml_io_map(W_PHY_PHYS, 0x1000);
	gDbi = (volatile uint32_t *)ml_io_map(W_DBI_PHYS, 0x1000);
	gConf = (volatile uint32_t *)ml_io_map(W_CONF_PHYS, 0x1000);
	if (gGcc && gTlmm && gBtMode >= 1) {
		B_LOG("mode %u (%s)", gBtMode, gBtMode == 1 ? "read-only probe" : gBtMode == 2 ?
		    "step 1: out of reset, the UART lines" : "step 2: the UART, HCI commands");
		bt_dump("before");
		if (gBtMode >= 3) {
			bt_step2();
		} else if (gBtMode == 2) {
			bt_step1();
		} else {
			bt_summary("probe only (bt=1): states above; bt=2 takes the chip out of reset");
		}
	}
	if (gMode == 0) {
		/* Bluetooth only */
	} else if (!gGcc || !gTlmm || !gParf || !gPhy || !gDbi || !gConf) {
		summary("FAIL: ml_io_map");
	} else {
		W_LOG("mode %u (%s)", gMode, gMode == 1 ? "read-only probe" : gMode == 2 ? "step 1 (power, link)" :
		    gMode == 3 ? "steps 1, 2a (chip registers)" : gMode == 4 ? "steps 1, 2a + 2b (DMA, BMI)" :
		    gMode == 5 ? "steps 1-2b + 2c (firmware boot)" : gMode == 6 ? "steps 1-2c + 3 (WMI, HTT)" :
		    gMode == 7 ? "steps 1-3 + 4 (listen-only scan)" : gMode == 8 ? "steps 1-4 + 5 (join)" :
		    gMode == 9 ? "steps 1-5 + 6 (WPA2)" : gMode == 10 ? "steps 1-6 + 7 (DHCP, ping)" :
		    gMode == 11 ? "steps 1-7 + 8 (wl0 for iOS, stays connected)" : "steps 1-4 + the Wi-Fi manager (op2netd)");
		dump_all("before");
		if (gMode >= 2) {
			bringup();
			dump_all("after");
		} else {
			summary("probe only (wifi=1): states above; wifi=2 attempts the bring-up");
		}
	}
	for (;;) {
		IOSleep(60000);
		if (gMode != 0) {
			W_LOG_CONSOLE("RESULT (repeat): %s", gSummary);
		}
		if (gBtMode != 0) {
			IOLog("OP2 BT: RESULT (repeat): %s\n", gBtSummary);
		}
	}
}

extern "C" void
op2_wifi_start(void)
{
	uint32_t mode = 0, bt = 0;
	PE_parse_boot_argn("wifi", &mode, sizeof(mode));
	PE_parse_boot_argn("bt", &bt, sizeof(bt));
	if (mode == 0 && bt == 0) {
		return;
	}
	gMode = mode;
	gBtMode = bt;
	thread_t th;
	if (kernel_thread_start(wifi_thread, NULL, &th) == KERN_SUCCESS) {
		thread_deallocate(th);
		IOLog("OP2 WIFI: armed, wifi=%u bt=%u, runs 120 s from now (Bluetooth first)\n", mode, bt);
	}
}

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
