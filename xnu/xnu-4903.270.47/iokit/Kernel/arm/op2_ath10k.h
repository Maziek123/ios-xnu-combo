/*
 * OP2Ath10k.cpp: Wi-Fi step 2b (QCA6174 copy engines, diagnostic window, first BMI command).
 * Called by OP2Wifi.cpp with boot-arg wifi=3 (wifi=4: and step 2c, the firmware boot; wifi=5: and step 3, the
 * firmware's HTC/WMI/HTT services up to WMI ready; wifi=6: and step 4, a listen-only scan) once step 2a has read a valid chip ID.
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
	bool start_services;       /* wifi=5: after a 2c pass, step 3 (HTC services, WMI init + ready, HTT version) */
	bool scan;                 /* wifi=6: after a step 3 pass, step 4 (a scan that only listens) */
	bool join;                 /* wifi=7: after a step 4 pass, step 5 (join the configured network) */
	bool wpa2;                 /* wifi=8: after a step 5 pass, step 6 (WPA2-PSK 4-way handshake, keys, 5 s) */
	bool data;                 /* wifi=9: after a step 6 pass, step 7 (DHCP through the keys, ARP, ping the router) */
};

/* Runs step 2b (and 2c, 3); writes the last step's one-line result ("STEP 2b PASS: ...", "STEP 3 FAIL: ...")
 * into result. */
extern "C" bool op2_ath10k_step2b(const struct op2_ath10k_bus *bus, char *result, size_t len);

#endif /* _OP2_ATH10K_H_ */
