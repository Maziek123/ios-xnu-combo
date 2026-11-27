/*
 * OP2Wifid: toward the status-bar Wi-Fi icon and Settings' Wi-Fi switch (README "The status-bar Wi-Fi icon").
 */
#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_MSM8994)

#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSNumber.h>
#include "op2_ath10k.h"

/*
 * A stand-in for Apple's IO80211Interface (IO80211Family, not in this kernel). Apple's wifid waits for a service of
 * this class to be published (seen on the phone 2026-11-27: "IOServicePublish ... IO80211Interface -> waiting") and
 * draws the status-bar Wi-Fi icon and Settings' Wi-Fi switch from what it learns from it. This one only carries the
 * names wifid may look for (BSD Name wl0, the chip's MAC); what wifid asks of it next shows in the "IOK wifid:" and
 * "OP2 WIFID:" lines (IOUserClient.cpp, sys_generic.c). Published only with boot-arg op2wifid=2, when the Wi-Fi
 * manager comes up (OP2Ath10k.cpp run_manager). Same pattern as OP2Digitizer's IOHIDDevice stand-in.
 */
class IO80211Interface : public IOService {
	OSDeclareDefaultStructors(IO80211Interface);
};

OSDefineMetaClassAndStructors(IO80211Interface, IOService);

extern "C" void
op2_wifi_publish_80211(const uint8_t mac[6], char *out, size_t n)
{
	static bool done;
	uint32_t mode = 1;
	PE_parse_boot_argn("op2wifid", &mode, sizeof(mode));
	if (mode < 2) {
		snprintf(out, n, "wifid: no IO80211Interface for it (boot-arg op2wifid=2 publishes one)");
		return;
	}
	if (done) {
		snprintf(out, n, "wifid: IO80211Interface already published");
		return;
	}
	IO80211Interface *nub = OSTypeAlloc(IO80211Interface);
	IOService *res = IOService::getResourceService();
	if (nub == NULL || res == NULL || !nub->init()) {
		OSSafeReleaseNULL(nub);
		snprintf(out, n, "wifid: IO80211Interface: allocation failed");
		return;
	}
	nub->setName("wl0");
	nub->setProperty("BSD Name", "wl0");
	nub->setProperty("IOInterfaceName", "wl");
	nub->setProperty("IOInterfaceUnit", 0ULL, 32);
	nub->setProperty("IOMACAddress", (void *)mac, 6);
	if (!nub->attach(res)) {
		nub->release();
		snprintf(out, n, "wifid: IO80211Interface: attach failed");
		return;
	}
	nub->registerService();
	nub->release();   /* the registry keeps it */
	done = true;
	snprintf(out, n, "wifid: IO80211Interface published (wl0, %02x:%02x:%02x:%02x:%02x:%02x): its next requests show as "
	    "\"IOK wifid:\" and \"OP2 WIFID:\" lines", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
