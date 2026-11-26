/*
 * op2_wlan.c: wl0, the network interface for the QCA6174 Wi-Fi (Wi-Fi step 8, boot-arg wifi=11).
 *
 * The driver (iokit/Kernel/arm/OP2Ath10k.cpp) runs in its own polling kernel thread; this file is only the
 * BSD side, built the way ue0 is in op2_usb.c: an Ethernet-type ifnet whose output queues frames that the
 * driver's thread takes (op2_wlan_dequeue) and sends as 802.11 data frames, and op2_wlan_input for frames from
 * the network. op2_wlan_up gives wl0 the DHCP lease of step 7 and publishes it in the sysctl net.op2wlan
 * ("wl0 <address> <netmask> <router> <dns>", empty while down), from which op2shell makes the configd service
 * (State:/Network/Service/op2wifi) that gives iOS its default route and DNS.
 */
#if defined(ARM64_BOARD_CONFIG_MSM8994)

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/sockio.h>
#include <kern/locks.h>
#include <sys/kpi_mbuf.h>
#include <sys/kpi_socket.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/ethernet.h>
#include <net/if_ether.h>
#include <net/ether_if_module.h>
#include <net/kpi_interface.h>
#include <net/kpi_protocol.h>
#include <netinet/in.h>

int op2_wlan_attach(const uint8_t mac[6]);
int op2_wlan_up(uint32_t ip, uint32_t mask, uint32_t router, uint32_t dns);
void op2_wlan_down(void);
uint32_t op2_wlan_dequeue(uint8_t *frame, uint32_t max);
void op2_wlan_input(const uint8_t *frame, uint32_t len);
uint32_t op2_wlan_drops(void);

#define WL_TXQ 64

static ifnet_t wl_ifp;
static lck_mtx_t *wl_lock;   /* the transmit queue */
static mbuf_t wl_txq[WL_TXQ];
static unsigned wl_txq_head, wl_txq_count;
static int wl_running;
static uint32_t wl_drops, wl_ip;

static char wl_lease[96];
SYSCTL_STRING(_net, OID_AUTO, op2wlan, CTLFLAG_RD | CTLFLAG_LOCKED, wl_lease, 0,
    "Wi-Fi wl0 lease: name address netmask router dns (empty while down)");

static errno_t
wl_if_output(ifnet_t ifp, mbuf_t m)
{
	lck_mtx_lock(wl_lock);
	if (wl_txq_count == WL_TXQ || !wl_running) {
		wl_drops++;
		lck_mtx_unlock(wl_lock);
		mbuf_freem(m);
		ifnet_stat_increment_out(ifp, 0, 0, 1);
		return 0;
	}
	ifnet_stat_increment_out(ifp, 1, mbuf_pkthdr_len(m), 0);
	wl_txq[(wl_txq_head + wl_txq_count) % WL_TXQ] = m;
	wl_txq_count++;
	lck_mtx_unlock(wl_lock);
	return 0;
}

static errno_t
wl_if_ioctl(ifnet_t ifp, unsigned long cmd, __unused void *data)
{
	switch (cmd) {
	case SIOCSIFADDR:
	case SIOCAIFADDR:
		ifnet_set_flags(ifp, IFF_UP, IFF_UP);
		return 0;
	case SIOCSIFFLAGS:
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		return 0;
	default:
		return EOPNOTSUPP;
	}
}

static void
wl_if_detach(__unused ifnet_t ifp)
{
}

int
op2_wlan_attach(const uint8_t mac[6])
{
	struct ifnet_init_eparams init;
	lck_grp_t *grp;
	errno_t err;

	if (wl_ifp != NULL) {
		return 0;
	}
	grp = lck_grp_alloc_init("op2-wlan", LCK_GRP_ATTR_NULL);
	wl_lock = lck_mtx_alloc_init(grp, LCK_ATTR_NULL);
	bzero(&init, sizeof(init));
	init.ver = IFNET_INIT_CURRENT_VERSION;
	init.len = sizeof(init);
	init.flags = IFNET_INIT_LEGACY;
	init.uniqueid = "op2-wlan";
	init.uniqueid_len = sizeof("op2-wlan") - 1;
	init.name = "wl";
	init.unit = 0;
	init.family = IFNET_FAMILY_ETHERNET;
	init.type = IFT_ETHER;
	init.output = wl_if_output;
	init.demux = ether_demux;
	init.add_proto = ether_add_proto;
	init.del_proto = ether_del_proto;
	init.check_multi = ether_check_multi;
	init.framer_extended = ether_frameout_extended;
	init.ioctl = wl_if_ioctl;
	init.detach = wl_if_detach;
	init.broadcast_addr = etherbroadcastaddr;
	init.broadcast_len = ETHER_ADDR_LEN;
	err = ifnet_allocate_extended(&init, &wl_ifp);
	if (err != 0) {
		printf("OP2 WIFI: 8: wl0: ifnet_allocate failed (%d)\n", err);
		wl_ifp = NULL;
		return err;
	}
	ifnet_set_addrlen(wl_ifp, ETHER_ADDR_LEN);
	ifnet_set_hdrlen(wl_ifp, sizeof(struct ether_header));
	ifnet_set_mtu(wl_ifp, ETHERMTU);
	ifnet_set_baudrate(wl_ifp, 54000000);
	ifnet_set_flags(wl_ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST, 0xffff);
	err = ifnet_attach(wl_ifp, NULL);
	if (err != 0) {
		printf("OP2 WIFI: 8: wl0: ifnet_attach failed (%d)\n", err);
		wl_ifp = NULL;
		return err;
	}
	ifnet_set_lladdr(wl_ifp, mac, ETHER_ADDR_LEN);
	err = proto_plumb(PF_INET, wl_ifp);
	if (err != 0 && err != EEXIST) {
		printf("OP2 WIFI: 8: wl0: proto_plumb(PF_INET) failed (%d)\n", err);
		return err;
	}
	return 0;
}

static void
sin_set(struct sockaddr_in *s, uint32_t a)
{
	bzero(s, sizeof(*s));
	s->sin_len = sizeof(*s);
	s->sin_family = AF_INET;
	s->sin_addr.s_addr = htonl(a);
}

static void
ip_text(char *out, size_t n, uint32_t a)
{
	snprintf(out, n, "%u.%u.%u.%u", a >> 24, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff);
}

int
op2_wlan_up(uint32_t ip, uint32_t mask, uint32_t router, uint32_t dns)
{
	struct ifaliasreq ifra;
	struct sockaddr_in a4;
	socket_t so = NULL;
	errno_t err;
	char t[4][16];

	if (wl_ifp == NULL) {
		return ENXIO;
	}
	if (mask == 0) {
		mask = 0xffffff00u;
	}
	err = sock_socket(PF_INET, SOCK_DGRAM, 0, NULL, NULL, &so);
	if (err == 0) {
		bzero(&ifra, sizeof(ifra));
		snprintf(ifra.ifra_name, sizeof(ifra.ifra_name), "wl0");
		sin_set(&a4, ip);
		memcpy(&ifra.ifra_addr, &a4, sizeof(a4));
		sin_set(&a4, mask);
		memcpy(&ifra.ifra_mask, &a4, sizeof(a4));
		sin_set(&a4, ip | ~mask);
		memcpy(&ifra.ifra_broadaddr, &a4, sizeof(a4));
		err = sock_ioctl(so, SIOCAIFADDR, &ifra);
		sock_close(so);
	}
	if (err != 0) {
		printf("OP2 WIFI: 8: wl0: SIOCAIFADDR failed (%d)\n", err);
		return err;
	}
	lck_mtx_lock(wl_lock);
	wl_running = 1;
	wl_ip = ip;
	lck_mtx_unlock(wl_lock);
	ifnet_set_flags(wl_ifp, IFF_UP | IFF_RUNNING, IFF_UP | IFF_RUNNING);
	ip_text(t[0], sizeof(t[0]), ip);
	ip_text(t[1], sizeof(t[1]), mask);
	ip_text(t[2], sizeof(t[2]), router);
	ip_text(t[3], sizeof(t[3]), dns != 0 ? dns : router);
	snprintf(wl_lease, sizeof(wl_lease), "wl0 %s %s %s %s", t[0], t[1], t[2], t[3]);
	printf("OP2 WIFI: 8: wl0 up: net.op2wlan = \"%s\"\n", wl_lease);
	return 0;
}

void
op2_wlan_down(void)
{
	struct ifreq ifr;
	struct sockaddr_in a4;
	socket_t so = NULL;
	uint32_t ip;

	if (wl_ifp == NULL) {
		return;
	}
	wl_lease[0] = '\0';
	lck_mtx_lock(wl_lock);
	wl_running = 0;
	ip = wl_ip;
	wl_ip = 0;
	while (wl_txq_count > 0) {
		mbuf_freem(wl_txq[wl_txq_head]);
		wl_txq_head = (wl_txq_head + 1) % WL_TXQ;
		wl_txq_count--;
	}
	lck_mtx_unlock(wl_lock);
	ifnet_set_flags(wl_ifp, 0, IFF_RUNNING);
	if (ip != 0 && sock_socket(PF_INET, SOCK_DGRAM, 0, NULL, NULL, &so) == 0) {
		bzero(&ifr, sizeof(ifr));
		snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "wl0");
		sin_set(&a4, ip);
		memcpy(&ifr.ifr_addr, &a4, sizeof(a4));
		(void)sock_ioctl(so, SIOCDIFADDR, &ifr);
		sock_close(so);
	}
	printf("OP2 WIFI: 8: wl0 down (address removed, net.op2wlan empty)\n");
}

uint32_t
op2_wlan_dequeue(uint8_t *frame, uint32_t max)
{
	mbuf_t m = NULL;
	size_t len;

	if (wl_ifp == NULL) {
		return 0;
	}
	for (;;) {
		lck_mtx_lock(wl_lock);
		if (wl_txq_count == 0) {
			lck_mtx_unlock(wl_lock);
			return 0;
		}
		m = wl_txq[wl_txq_head];
		wl_txq_head = (wl_txq_head + 1) % WL_TXQ;
		wl_txq_count--;
		lck_mtx_unlock(wl_lock);
		len = mbuf_pkthdr_len(m);
		if (len >= ETHER_HDR_LEN && len <= max && mbuf_copydata(m, 0, len, frame) == 0) {
			mbuf_freem(m);
			return (uint32_t)len;
		}
		mbuf_freem(m);
		lck_mtx_lock(wl_lock);
		wl_drops++;
		lck_mtx_unlock(wl_lock);
	}
}

void
op2_wlan_input(const uint8_t *frame, uint32_t len)
{
	/* single mbuf (maxchunks 1), as ue0: the header split below needs it all in the first mbuf */
	unsigned int one = 1;
	mbuf_t m = NULL;

	if (wl_ifp == NULL || !wl_running || len < ETHER_HDR_LEN || len > ETHER_MAX_LEN) {
		return;
	}
	if (mbuf_allocpacket(MBUF_DONTWAIT, len, &one, &m) == 0 &&
	    mbuf_copyback(m, 0, len, frame, MBUF_DONTWAIT) == 0 && mbuf_len(m) >= ETHER_HDR_LEN) {
		struct ifnet_stat_increment_param stats = { 0 };
		void *frame_header = mbuf_data(m);
		mbuf_pkthdr_setrcvif(m, wl_ifp);
		mbuf_pkthdr_setheader(m, frame_header);
		mbuf_pkthdr_adjustlen(m, -ETHER_HDR_LEN);
		(void)mbuf_setdata(m, (char *)mbuf_data(m) + ETHER_HDR_LEN, mbuf_len(m) - ETHER_HDR_LEN);
		stats.packets_in = 1;
		stats.bytes_in = len;
		ifnet_input(wl_ifp, m, &stats);
		return;
	}
	if (m != NULL) {
		mbuf_freem(m);
	}
	ifnet_stat_increment_in(wl_ifp, 0, 0, 1);
}

uint32_t
op2_wlan_drops(void)
{
	return wl_drops;
}

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
