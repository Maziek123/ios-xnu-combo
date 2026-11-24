/*
 * OP2Ath10k.cpp: Wi-Fi step 2b (QCA6174 copy engines, diagnostic window, first BMI command).
 * Called by OP2Wifi.cpp with boot-arg wifi=3 (wifi=4: and step 2c, the firmware boot) once step 2a has read a
 * valid chip ID.
 * Include after <IOKit/IOLib.h>.
 */

#ifndef _OP2_ATH10K_H_
#define _OP2_ATH10K_H_

struct op2_ath10k_bus {
	volatile uint32_t *bar0;   /* chip BAR0 (2 MB), mapped, memory decoding on */
	volatile uint32_t *conf;   /* chip config space (iATU CFG0 window) */
	volatile uint32_t *dbi;    /* root port's own config space / DBI */
	uint64_t power_on_abs;     /* mach absolute time when WLAN_EN went high */
	/* ath10k cold reset (SOC_GLOBAL_RESET) with the link and config space checked around it (OP2Wifi.cpp);
	 * false = the chip may be off the bus: no further access to it */
	bool (*cold_reset)(char *why, size_t len);
	bool boot_firmware;        /* wifi=4: after a 2b pass, step 2c (firmware download, BMI_DONE, HTC ready) */
};

/* Runs step 2b; writes a one-line result ("STEP 2b PASS: ..." / "STEP 2b FAIL: ...") into result. */
extern "C" bool op2_ath10k_step2b(const struct op2_ath10k_bus *bus, char *result, size_t len);

#endif /* _OP2_ATH10K_H_ */
