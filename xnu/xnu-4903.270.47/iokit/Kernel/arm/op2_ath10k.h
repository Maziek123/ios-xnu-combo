/*
 * OP2Ath10k.cpp: Wi-Fi step 2b (QCA6174 copy engines, diagnostic window, first BMI command).
 * Called by OP2Wifi.cpp with boot-arg wifi=4 and up once step 2a has read a valid chip ID; the flags below say
 * which later steps run (wifi=5: step 2c, the firmware boot ... wifi=11: step 8, the interface; wifi=12: the
 * Wi-Fi manager after step 4).
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
	bool boot_firmware;        /* wifi=5: after a 2b pass, step 2c (firmware download, BMI_DONE, HTC ready) */
	bool start_services;       /* wifi=6: after a 2c pass, step 3 (HTC services, WMI init + ready, HTT version) */
	bool scan;                 /* wifi=7: after a step 3 pass, step 4 (a scan that only listens) */
	bool join;                 /* wifi=8: after a step 4 pass, step 5 (join the configured network) */
	bool wpa2;                 /* wifi=9: after a step 5 pass, step 6 (WPA2-PSK 4-way handshake, keys, 5 s) */
	bool data;                 /* wifi=10: after a step 6 pass, step 7 (DHCP through the keys, ARP, ping the router) */
	bool iface;                /* wifi=11: after a step 7 pass, step 8 (wl0 for iOS on the lease; stays connected) */
	uint32_t iface_ms;         /* step 8 / manager: how long to stay (0: until the network ends it or the phone resets) */
	bool manager;              /* wifi=12: after step 4, the Wi-Fi manager (scan and join at runtime, driven by op2netd) */
};

/* wl0 (bsd/dev/op2_wlan.c), used by step 8. Addresses as numbers (172.20.10.6 = 0xAC140A06). */
extern "C" int op2_wlan_attach(const uint8_t mac[6]);          /* create wl0 (once); 0 or an errno */
extern "C" int op2_wlan_up(uint32_t ip, uint32_t mask, uint32_t router, uint32_t dns);   /* address, up, sysctl */
extern "C" void op2_wlan_down(void);                           /* sysctl cleared, address removed, queue dropped */
extern "C" uint32_t op2_wlan_dequeue(uint8_t *frame, uint32_t max);   /* next Ethernet frame from iOS, 0: none */
extern "C" void op2_wlan_input(const uint8_t *frame, uint32_t len);    /* an Ethernet frame for iOS */
extern "C" uint32_t op2_wlan_drops(void);                      /* frames iOS queued that were dropped */
extern "C" void op2_wlan_set_state(const char *text);          /* sysctl net.op2wlan_state (Wi-Fi manager) */
extern "C" void op2_wlan_set_scan(const char *text);           /* sysctl net.op2wlan_scan */
extern "C" int op2_wlan_take_cmd(char *out, uint32_t n);       /* the newest net.op2wlan_cmd, once: 1, else 0 */
/* bsd/kern/op2_shell_sync.c: what the boot-time op2shell check did, and whether op2-air-run (op2shell) runs */
extern "C" void op2_shell_status(char *out, size_t n);
/* bsd/dev/op2_keeplog.c: the line also into the kept log (a ramoops zone of its own, safe from console floods) */
extern "C" void op2_keep(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Runs step 2b (and 2c, 3); writes the last step's one-line result ("STEP 2b PASS: ...", "STEP 3 FAIL: ...")
 * into result. */
extern "C" bool op2_ath10k_step2b(const struct op2_ath10k_bus *bus, char *result, size_t len);

#endif /* _OP2_ATH10K_H_ */
