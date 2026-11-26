/* Host-side simulation of the QCA6174 for OP2Ath10k.cpp (steps 2b to 6; the access point is in ap.inc). Not kernel code.
 * 0: ROM initialized at 300 ms by itself
 * 1: ROM initialized only once PCIE_INTR_ENABLE has the firmware bit (50 ms later)
 * 2: ROM silent until a cold reset (copy engines cleared by it), initialized 60 ms after release
 * 3: ROM silent for good
 * 4: ROM silent and the cold reset fails (link down)
 * 5: as 2, but the cold reset leaves the copy-engine registers alone (pending descriptors survive)
 * 6: step 2c too (boot_firmware): BMI download, BMI_DONE, HTC ready 50 ms later
 * 7: step 2c, the firmware crashes after BMI_DONE (crash record at hi_failure_state)
 * 8: step 2c, the firmware stays silent after BMI_DONE
 * 9: step 3 too: HTC connects, service ready (an unrequested debug event first), init, ready (credit back in its
 *    trailer), HTT version, echo
 * 10: step 3 with two host memory requests (one sized by the number of peers)
 * 11: step 3, the firmware never sends WMI ready
 * 12: step 3, the firmware crashes after WMI init
 * 13: step 3 with 1 credit: the credit for init comes back 20 ms after ready, in a trailer-only message on endpoint 0
 * 14: step 3, the firmware refuses the HTT service
 * 15: step 3, the service ready event has another WMI ABI namespace
 * 16: step 3, WMI ready only once the chip's interrupt enable is on
 * 17: step 3, the firmware stops after WMI init leaving a register dump but no crash flag (as on the phone)
 * 18: step 4 too: regdomain, channel list, vdev create/delete/create, passive scan with beacons from 3 networks
 * 19: step 4, the firmware reports "start failed" for the scan
 * 20: step 4, beacons only through the HTT rx ring (300 frames: more than the 127 posted buffers, so the host must
 *     post them again), a data frame and a frame with only rx_hdr_status; channel reports for every channel
 * 21: step 4, nothing heard at all: channel reports only (the FAIL line carries the radio numbers)
 * (11 also checks the long wait, the echo after it and the firmware debug log dump; every 2c run checks the PLL
 * switch and the OTP board-id run)
 * BMI answers only while the ROM is initialized; requests stay pending otherwise (as on the phone). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include "IOKit/IOLib.h"
#include "kern/clock.h"
#include "op2_ath10k.h"
#include <vector>
#include <deque>
#include <map>
#include <string>
#include <algorithm>
#include <utility>
#include "op2_wifi_fw.h"   /* the same generated header OP2Ath10k.cpp sees (real firmware-6.bin) */

#define DMA_BYTES 0x20000u
static uint32_t bar[0x200000 / 4], conf[0x1000 / 4], dbi[0x1000 / 4];
static uint8_t *dma;
static const uint64_t kDmaPhys = 0x80000000ull, kChunkPhys = 0x90000000ull;
struct chunk { uint64_t phys; size_t size; uint8_t *mem; };
static std::vector<chunk> &chunks = *new std::vector<chunk>;   /* never destroyed: the firmware may still use it */
static uint8_t *region[16];   /* target memory, 1 MB per region; region 4 = DRAM 0x400000 */
#define tram (region[4])
static uint64_t now_us, intr_at, released_at, bmi_done_at, credit_due_at;
static bool lz_on, crashed, ready_sent;
static std::vector<uint8_t> lz_cur, lz_last;
static uint32_t exec_addr = 0xffffffffu, exec_param = 0xffffffffu, app_start, lz_streams;
static int scenario, cold_resets, answers;
static bool in_reset;
static uint32_t win;   /* CORE_CTRL window base (bits 10:0 << 21) */

/* step 3 firmware model */
static int fw_credits, init_credits;             /* WMI credits the host may still use, as the firmware counts them */
static std::deque<std::vector<uint8_t>> outq[8];  /* messages waiting for a host receive buffer, per copy engine */
static std::vector<uint8_t> init_cmd;             /* the WMI init the host sent (TLVs) */
static uint32_t seq_seen[9], connects, setup_completes, inits, echos, version_reqs, echo_value;
static bool init_seen, ready_queued_with_intr;
static std::map<uint32_t, uint32_t> soc;          /* SoC registers reached through BMI_READ/WRITE_SOC_REGISTER */
static int pll_changing;                          /* RTC_SYNC_STATUS reads that still show PLL_CHANGING */
static std::vector<uint32_t> exec_params;
static uint32_t directed_scans, chanlist_active, chanlists, stuck_buffers;
static uint32_t scan_flags, scan_nchan, chanlist_n, chanlist_passive, regd_rd, vdev_creates, vdev_deletes, starts;
static uint8_t vdev_mac[6];
/* HTT rx ring as the firmware sees it */
static const uint64_t kRxPhys = 0xA0000000ull;
static uint8_t *rxpool;
static size_t rxpool_size;
static uint32_t ring_cfgs, aggr_cfgs, ring_base, ring_idx_pa, ring_size, ring_fw_rd, ring_written, rx_inds;
static bool ring_cfg_before_regd, regd_seen, params_before_regd;
static std::vector<std::pair<uint32_t, uint32_t>> &pdev_params = *new std::vector<std::pair<uint32_t, uint32_t>>;

#define CE(n) ((0x34400u + (n) * 0x400u) / 4)
static uint32_t *reg(unsigned ce, unsigned off) { return &bar[CE(ce) + off / 4]; }

static uint8_t *
resolve(uint32_t a, uint32_t n)
{
	if (a >= kDmaPhys && (uint64_t)a + n <= kDmaPhys + DMA_BYTES) {
		return dma + (a - kDmaPhys);
	}
	for (auto &c : chunks) {
		if (a >= c.phys && (uint64_t)a + n <= c.phys + c.size) return c.mem + (a - c.phys);
	}
	if (rxpool && a >= kRxPhys && (uint64_t)a + n <= kRxPhys + rxpool_size) {
		return rxpool + (a - kRxPhys);
	}
	if ((a & 0xffe00000u) == win) {
		uint32_t r = (a & 0x100000u) ? (bar[0x3a018 / 4] & 0xfu) : 0u;   /* CPU_ADDR_MSB window or region 0 */
		uint32_t o = a & 0xfffffu;
		if (o + n <= 0x100000u) {
			if (!region[r]) region[r] = (uint8_t *)calloc(1, 0x100000);
			return region[r] + o;
		}
	}
	fprintf(stderr, "SIM: bad DMA address %08x (+%u)\n", a, n);
	abort();
}

static uint8_t *
tmem(uint32_t a, uint32_t n)   /* target CPU address */
{
	uint32_t r = (a >> 20) & 0xfu, o = a & 0xfffffu;
	assert(o + n <= 0x100000u);
	if (!region[r]) region[r] = (uint8_t *)calloc(1, 0x100000);
	return region[r] + o;
}

static uint32_t *desc(uint32_t base, uint32_t i) { return (uint32_t *)(void *)resolve(base + i * 8u, 8u); }

static bool
initialized(void)
{
	switch (scenario) {
	case 1: return intr_at && now_us >= intr_at + 50000;
	case 2: case 5: return released_at && now_us >= released_at + 60000;
	case 3: case 4: return false;
	default: return now_us >= 300000;
	}
}

/* The target writes one message into the next receive buffer the host posted on copy engine ce. */
static bool
deliver(unsigned ce, const std::vector<uint8_t> &m)
{
	uint32_t di = *reg(ce, 0x48);
	if (di == *reg(ce, 0x40)) {
		return false;   /* no buffer posted: the target's send waits */
	}
	uint32_t *d = desc(*reg(ce, 0x08), di);
	assert((d[1] & 0xffffu) == 0);
	memcpy(resolve(d[0], (uint32_t)m.size()), m.data(), m.size());
	d[1] = (d[1] & 0xffff0000u) | (uint32_t)m.size();
	*reg(ce, 0x48) = (di + 1) % *reg(ce, 0x0c);
	return true;
}

static std::vector<uint8_t>
htc(uint8_t eid, const std::vector<uint8_t> &payload, const std::vector<uint8_t> &trailer = {})
{
	size_t plen = payload.size() + trailer.size();
	std::vector<uint8_t> m = { eid, (uint8_t)(trailer.empty() ? 0 : 2), (uint8_t)plen, (uint8_t)(plen >> 8),
		(uint8_t)trailer.size(), 0, 0, 0 };
	m.insert(m.end(), payload.begin(), payload.end());
	m.insert(m.end(), trailer.begin(), trailer.end());
	return m;
}

static void put32(std::vector<uint8_t> &v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i))); }
static void tlv(std::vector<uint8_t> &v, uint16_t tag, const std::vector<uint32_t> &words)
{
	put32(v, (uint32_t)(words.size() * 4) | ((uint32_t)tag << 16));
	for (uint32_t w : words) put32(v, w);
}
static std::vector<uint8_t> credit_trailer(uint8_t eid, uint8_t n) { return { 1, 4, 0, 0, eid, n, 0, 0 }; }

static void
queue_service_ready(void)
{
	std::vector<uint8_t> p;
	put32(p, 0x1u);   /* WMI_TLV_SERVICE_READY_EVENTID */
	uint32_t nreq = scenario == 10 ? 2u : 0u;
	std::vector<uint32_t> ev = { 0x0000f0a1u, 0x01000000u, 7u, scenario == 15 ? 0x5F414352u : 0x5F414351u, 0x4C4Du, 0, 0,
		0x3u, 0, 2u, 0x1f0bfu, 0x339959b2u, 0xfffafffau, 0, 63u, 0, 0, 0, nreq, 64u, 0, 0, 0, 0, 0, 0 };
	tlv(p, 32, ev);
	tlv(p, 33, { 0x6au, 0, 0x1fu, 0x1du, 0x1fu, 2312u, 2732u, 4912u, 6100u });
	tlv(p, 16, { 0x1u, 0x0u, 0x2u, 0x0u });   /* bit 65: rx full reorder */
	std::vector<uint8_t> reqs;
	if (nreq) {
		tlv(reqs, 34, { 1u, 2214u, 2u, 0u });    /* units = peers + 1, size rounded up to 2216 */
		tlv(reqs, 34, { 2u, 256u, 0u, 12u });
	}
	put32(p, (uint32_t)reqs.size() | (18u << 16));
	p.insert(p.end(), reqs.begin(), reqs.end());
	outq[2].push_back(htc(2, p));
}

static void
queue_ready(bool trailer)
{
	std::vector<uint8_t> p;
	put32(p, 0x2u);   /* WMI_TLV_READY_EVENTID */
	tlv(p, 35, { 0x01000000u, 53u, 0x5F414351u, 0x4C4Du, 0, 0, 0x127f0300u, 0x00005634u, 0u });   /* MAC 00:03:7f:12:34:56 */
	outq[2].push_back(htc(2, p, trailer ? credit_trailer(2, 1) : std::vector<uint8_t>()));
}

static void
queue_scan_event(uint32_t type, uint32_t reason, uint32_t freq)
{
	std::vector<uint8_t> p;
	put32(p, 0x3001u);
	tlv(p, 36, { type, reason, freq, 0xA001u, 0xA000u, 0 });
	outq[2].push_back(htc(2, p));
}

static std::vector<uint8_t>
make_beacon(const uint8_t bssid[6], const char *ssid, uint8_t chan, bool rsn, uint8_t subtype)
{
	std::vector<uint8_t> f = { subtype, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	f.insert(f.end(), bssid, bssid + 6);
	f.insert(f.end(), bssid, bssid + 6);
	f.insert(f.end(), { 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0x64, 0, (uint8_t)(rsn ? 0x11 : 0x01), 0x04 });
	size_t sl = strlen(ssid);
	f.push_back(0); f.push_back((uint8_t)sl); f.insert(f.end(), ssid, ssid + sl);
	if (chan) f.insert(f.end(), { 3, 1, chan });
	if (rsn) f.insert(f.end(), { 48, 2, 1, 0 });
	return f;
}

static void
queue_beacon(const uint8_t bssid[6], const char *ssid, uint8_t chan, uint32_t snr, bool rsn, uint32_t status, uint8_t subtype)
{
	std::vector<uint8_t> f = make_beacon(bssid, ssid, chan, rsn, subtype);
	uint32_t blen = (uint32_t)f.size();
	while (f.size() % 4) f.push_back(0);
	std::vector<uint8_t> p;
	put32(p, 0x7001u);
	tlv(p, 44, { chan, snr, 0, 0, blen, status, 0, 0, 0, 0 });
	put32(p, (uint32_t)f.size() | (17u << 16));
	p.insert(p.end(), f.begin(), f.end());
	outq[2].push_back(htc(2, p));
}

/* A frame for the HTT rx ring, kept in the WMI queue so it keeps its place among the scan events. Marker 0xFE,
 * then attention, msdu length, rssi, whether the payload carries the frame, then the frame. */
static void
queue_ring_frame(const std::vector<uint8_t> &f, uint32_t attn, bool in_payload, uint8_t rssi, uint8_t kind = 0xFE)
{
	/* kind: 0xFE descriptor + MSDU_DONE (rx indication); 0xFD the same, announced by an in-order indication;
	 * 0xFC offloaded (short header, no descriptor) + in-order indication; 0xFB the chip takes a buffer and never
	 * finishes it */
	std::vector<uint8_t> m = { kind };
	put32(m, attn);
	put32(m, in_payload ? (uint32_t)f.size() : 0u);
	m.push_back(rssi);
	m.push_back(in_payload ? 1 : 0);
	m.insert(m.end(), f.begin(), f.end());
	outq[2].push_back(m);
}

/* The chip writes one frame into the next posted buffer, then sends an rx indication; false while no buffer. */
static bool
ring_put(const std::vector<uint8_t> &m)
{
	assert(ring_cfgs == 1);
	uint32_t alloc; memcpy(&alloc, resolve(ring_idx_pa, 4), 4);
	assert(alloc < ring_size);
	if (ring_fw_rd == alloc) {
		return false;
	}
	uint32_t pa; memcpy(&pa, resolve(ring_base + 4u * ring_fw_rd, 4), 4);
	uint8_t *d = resolve(pa, 2048);
	uint32_t attn; memcpy(&attn, d + 4, 4);
	assert(attn == 0);                    /* the host clears the descriptor before posting a buffer */
	uint32_t a, mlen; memcpy(&a, &m[1], 4); memcpy(&mlen, &m[5], 4);
	uint8_t rssi = m[9], kind = m[0];
	bool in_payload = m[10] != 0;
	const uint8_t *f = &m[11];
	size_t fl = m.size() - 11;
	ring_fw_rd = (ring_fw_rd + 1) & (ring_size - 1);
	ring_written++;
	if (kind == 0xFB) {
		stuck_buffers++;
		return true;
	}
	std::vector<uint8_t> ind = { 0x12, 0, 0x96, 0, 0, 0, 1, 0 };   /* in-order: info, peer 150, vdev 0, 1 buffer */
	for (int i = 0; i < 4; i++) ind.push_back((uint8_t)(pa >> (8 * i)));
	ind.push_back((uint8_t)fl); ind.push_back((uint8_t)(fl >> 8)); ind.push_back(0); ind.push_back(0);
	if (kind == 0xFC) {                     /* struct htt_rx_offload_msdu + the native wifi frame */
		d[0] = (uint8_t)fl; d[1] = (uint8_t)(fl >> 8); d[2] = 0x96; d[3] = 0; d[4] = 0; d[5] = 0; d[6] = 0;
		memcpy(d + 7, f, fl);
		ind[1] = 0x20;
		outq[1].push_back(htc(1, ind));
		return true;
	}
	memcpy(d + 24, &mlen, 4);
	d[100] = rssi;
	memcpy(d + 236, f, fl < 64 ? fl : 64);
	if (in_payload) memcpy(d + 300, f, fl);
	memcpy(d + 4, &a, 4);
	if (kind == 0xFD) {
		outq[1].push_back(htc(1, ind));
	} else {
		outq[1].push_back(htc(1, { 1, 0, 0, 0, 1, 0, 0, 0 }));   /* HTT_T2H_MSG_TYPE_RX_IND (contents not used) */
	}
	return true;
}

static void
queue_chan_info(uint32_t mhz, uint32_t flags, int32_t nf, uint32_t rx_clear, uint32_t cycles, uint32_t rx_frame)
{
	std::vector<uint8_t> p;
	put32(p, 0x4002u);
	tlv(p, 38, { 0, mhz, flags, (uint32_t)nf, rx_clear, cycles, 0, 0, rx_frame, 0, 0, 0, 160 });
	outq[2].push_back(htc(2, p));
}

#include "ap.inc"

/* ---- step 8: a stand-in for iOS's network stack on wl0 (bsd/dev/op2_wlan.c on the phone) ---- */
static struct {
	bool attached, up, down;
	uint8_t mac[6], router_mac[6];
	uint32_t ip, mask, router, dns;
	bool router_known, final_sent;
} st;
static std::deque<std::vector<uint8_t>> &st_out = *new std::deque<std::vector<uint8_t>>;
static uint32_t st_in, st_echo_replies, st_ipv6, st_own, st_arp_answered, st_bad;

static std::vector<uint8_t>
st_eth(const uint8_t *dst, uint16_t type, const std::vector<uint8_t> &payload)
{
	std::vector<uint8_t> e(dst, dst + 6);
	e.insert(e.end(), kStaMac, kStaMac + 6);
	be16put(e, type);
	e.insert(e.end(), payload.begin(), payload.end());
	return e;
}

static void
st_ping(uint16_t id, uint16_t seq, size_t data)
{
	std::vector<uint8_t> icmp = { 8, 0, 0, 0, (uint8_t)(id >> 8), (uint8_t)id, (uint8_t)(seq >> 8), (uint8_t)seq };
	for (size_t i = 0; i < data; i++) icmp.push_back((uint8_t)i);
	uint16_t c = ap_csum(icmp.data(), icmp.size());
	icmp[2] = (uint8_t)(c >> 8); icmp[3] = (uint8_t)c;
	st_out.push_back(st_eth(st.router_mac, 0x0800, ap_ipv4(1, st.ip, st.router, icmp)));
}

extern "C" int
op2_wlan_attach(const uint8_t mac[6])
{
	assert(!st.attached);
	st.attached = true;
	memcpy(st.mac, mac, 6);
	return 0;
}

extern "C" int
op2_wlan_up(uint32_t ip, uint32_t mask, uint32_t router, uint32_t dns)
{
	assert(st.attached && !st.up);
	st.up = true;
	st.ip = ip; st.mask = mask; st.router = router; st.dns = dns;
	std::vector<uint8_t> q = { 0, 1, 8, 0, 6, 4, 0, 1 };   /* who has the router */
	q.insert(q.end(), kStaMac, kStaMac + 6); be32put(q, ip);
	q.insert(q.end(), 6, 0); be32put(q, router);
	st_out.push_back(st_eth(kBcast_sim, 0x0806, q));
	return 0;
}

extern "C" void op2_wlan_down(void) { assert(st.up); st.down = true; }
extern "C" uint32_t op2_wlan_drops(void) { return 0; }
extern "C" void op2_shell_status(char *out, size_t n) { snprintf(out, n, "simulated; op2-air-run running (pid 1)"); }

extern "C" uint32_t
op2_wlan_dequeue(uint8_t *frame, uint32_t max)
{
	assert(st.up && !st.down);
	if (st_out.empty()) return 0;
	std::vector<uint8_t> e = st_out.front();
	st_out.pop_front();
	assert(e.size() <= max);
	memcpy(frame, e.data(), e.size());
	return (uint32_t)e.size();
}

extern "C" void
op2_wlan_input(const uint8_t *e, uint32_t n)
{
	assert(st.up && !st.down && n >= 14);
	st_in++;
	if (memcmp(e + 6, kStaMac, 6) == 0) { st_own++; return; }
	uint16_t type = (uint16_t)(e[12] << 8 | e[13]);
	const uint8_t *p = e + 14;
	if (type == 0x0806 && n >= 14 + 28) {
		uint32_t spa = (uint32_t)p[14] << 24 | p[15] << 16 | p[16] << 8 | p[17], tpa = (uint32_t)p[24] << 24 | p[25] << 16 | p[26] << 8 | p[27];
		if (p[7] == 2 && spa == st.router && memcmp(e, kStaMac, 6) == 0) {
			memcpy(st.router_mac, p + 8, 6);
			st.router_known = true;
			if (scenario == 33) {
				for (uint16_t i = 1; i <= 20; i++) st_ping(0x5354, i, 1400);
			} else {
				st_ping(0x5354, 1, 56);
				std::vector<uint8_t> odd(46, 0);                 /* an 802.3 frame (length field): the driver drops it */
				st_out.push_back(st_eth(st.router_mac, 46, odd));
			}
		} else if (p[7] == 1 && tpa == st.ip) {
			std::vector<uint8_t> r = { 0, 1, 8, 0, 6, 4, 0, 2 };
			r.insert(r.end(), kStaMac, kStaMac + 6); be32put(r, st.ip);
			r.insert(r.end(), p + 8, p + 14); be32put(r, spa);
			st_out.push_back(st_eth(p + 8, 0x0806, r));
			st_arp_answered++;
		} else {
			st_bad++;
		}
		return;
	}
	if (type == 0x0800 && n >= 14 + 28 && p[9] == 1 && p[20] == 0 && p[24] == 0x53 && p[25] == 0x54) {
		st_echo_replies++;
		if (scenario == 33 && st_echo_replies == 20 && !st.final_sent) {
			st.final_sent = true;
			st_ping(0xd0e1, 99, 8);
		}
		return;
	}
	if (type == 0x86dd) { st_ipv6++; return; }
	st_bad++;
}

/* One host->target message on copy engine ce (HTC control, WMI or HTT), as the firmware sees it. */
static void
fw_receive(unsigned ce, const uint8_t *m, uint32_t n, uint32_t dflags)
{
	assert(n >= 8);
	uint8_t eid = m[0], flags = m[1];
	uint32_t plen = m[2] | (m[3] << 8);
	assert(plen + 8 == n && m[4] == 0 && m[6] == 0 && m[7] == 0);
	const uint8_t *p = m + 8;
	if (ce == 4 && plen >= 1 && p[0] == 1) {                 /* HTT TX_FRM: transfer id 0, gathered, no HTC sequence */
		assert(dflags == 1 && eid == 1 && flags == 0 && m[5] == 0);
		ap_tx_frm(p, plen);
		return;
	}
	assert(dflags == ((uint32_t)eid << 2));                  /* transfer id = endpoint */
	assert(m[5] == seq_seen[eid]++);                          /* per-endpoint sequence numbers */
	if (ce == 0) {                                            /* HTC control */
		assert(eid == 0 && flags == 0);
		uint32_t id = p[0] | (p[1] << 8);
		if (id == 2) {
			assert(plen == 8);
			uint32_t svc = p[2] | (p[3] << 8), cf = p[4] | (p[5] << 8);
			connects++;
			uint8_t status = 0, ep = 0;
			if (svc == 0x0300) { assert(cf == 0x0008); ep = 1; if (scenario == 14) status = 1; }
			else if (svc == 0x0100) { assert(cf == (uint32_t)init_credits << 8); ep = 2; }
			else assert(!"unknown service");
			outq[1].push_back(htc(0, { 3, 0, (uint8_t)svc, (uint8_t)(svc >> 8), status, ep, 0x00, 0x07 }));
		} else if (id == 5) {
			assert(plen == 12);
			for (uint32_t i = 2; i < 12; i++) assert(p[i] == 0);
			setup_completes++;
			if (scenario == 9) {                                      /* a debug event nobody asked for */
				std::vector<uint8_t> dbg;
				put32(dbg, 0x1d003u);
				tlv(dbg, 17, { 0x11111111u, 0x22222222u });
				outq[2].push_back(htc(2, dbg));
			}
			queue_service_ready();
		} else {
			assert(!"unknown HTC control message");
		}
		return;
	}
	if (ce == 3) {                                            /* WMI */
		assert(eid == 2 && flags == 1);
		assert(fw_credits > 0);                               /* the host must never send without a credit */
		fw_credits--;
		uint32_t id; memcpy(&id, p, 4);
		if (id == 0x1u) {
			inits++;
			init_seen = true;
			init_cmd.assign(p + 4, p + plen);
			if (scenario == 12) { crashed = true; return; }
			if (scenario == 17) { uint32_t v = 0x05030000u; memcpy(tram + 0x35000, &v, 4); return; }   /* register dump, no indicator */
			if (scenario == 11) return;
			if (scenario == 13) { queue_ready(false); credit_due_at = now_us + 20000; return; }
			if (scenario == 16) return;   /* sent once PCIE_INTR_ENABLE has the firmware bit (chip_step) */
			queue_ready(true);
			fw_credits++;
		} else if (id == 0x1d001u) {
			echos++;
			uint32_t hdr, v; memcpy(&hdr, p + 4, 4); memcpy(&v, p + 8, 4);
			assert(hdr == (4u | (149u << 16)) && plen == 12);
			echo_value = v;
			std::vector<uint8_t> ev;
			put32(ev, 0x1d001u);
			tlv(ev, 54, { v });
			outq[2].push_back(htc(2, ev, credit_trailer(2, 1)));
			fw_credits++;
		} else if (id == 0x4003u) {                          /* PDEV_SET_PARAM */
			uint32_t v[4]; memcpy(v, p + 4, 16);
			assert(plen == 20 && v[0] == (12u | (82u << 16)) && v[1] == 0 && ring_cfgs == 1 && !regd_seen);
			pdev_params.push_back({ v[2], v[3] });
			outq[1].push_back(htc(0, {}, credit_trailer(2, 1)));
			fw_credits++;
		} else if (id == 0x4001u || id == 0x3003u || id == 0x5001u || id == 0x5002u || id == 0x3001u) {
			const uint8_t *t = p + 4;
			uint32_t w0; memcpy(&w0, t, 4);
			uint32_t tag = w0 >> 16;
			std::vector<uint32_t> v((plen - 4) / 4);
			memcpy(v.data(), t, v.size() * 4);
			if (id == 0x4001u) {
				assert(tag == 81 && (w0 & 0xffff) == 24); regd_rd = v[2]; assert(v[5] == 0xff && v[6] == 0xff);
				ring_cfg_before_regd = ring_cfgs == 1 && aggr_cfgs == 1 && vdev_creates == 1 && vdev_deletes == 1;
				params_before_regd = !pdev_params.empty();
				regd_seen = true;
			}
			if (id == 0x5001u) { assert(tag == 86 && v[2] == 2); vdev_creates++; memcpy(vdev_mac, &v[4], 6); }
			if (id == 0x5002u) { assert(tag == 87); vdev_deletes++; }
			if (id == 0x3003u) {
				assert(tag == 79);
				chanlist_n = v[1];
				chanlist_passive = chanlist_active = 0;
				chanlists++;
				for (uint32_t i = 0; i < chanlist_n; i++) {
					uint32_t *c = &v[3 + i * 7];
					assert((c[0] >> 16) == 80 && (c[0] & 0xffff) == 24);
					if (c[4] & (1u << 7)) chanlist_passive++; else chanlist_active++;
				}
			}
			if (id == 0x3001u) {
				assert(tag == 77 && (w0 & 0xffff) == 100);
				starts++;
				scan_flags = v[15];
				scan_nchan = v[17];
				assert((v[1] & 0xF000u) == 0xA000u && v[2] == v[1] + 1 && v[18] == 1);
				assert(v[1] == 0xA000u + 2 * (starts - 1));
				bool directed = v[15] == 0x30;
				if (directed) {   /* step 5: 2.4 GHz channels 1-13, the network's name, our rates; 1-11 active */
					assert(v[17] == 13 && v[19] == 1 && v[20] == 16 && v[6] == 100 && chanlist_active == 11);
					assert(v[26] == ((16u << 16) | 52) && v[27] == 2412 && v[39] == 2472);
					assert(v[40] == ((19u << 16) | 36) && v[41] == 6 && memcmp(&v[42], "iPhone", 6) == 0);
					assert(v[50] == ((19u << 16) | 8) && v[53] == ((17u << 16) | 16) && v.size() == 58);
					static const uint8_t ies[16] = { 1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 50, 4, 0x30, 0x48, 0x60, 0x6c };
					assert(memcmp(&v[54], ies, 16) == 0);
					directed_scans++;
				} else {
					assert(v[15] == 0x31 && v[17] == 37 && v[19] == 0 && v[20] == 0);
				}
				if (ap_scenario()) {
					queue_scan_event(1, 0, 0);
					queue_scan_event(8, 0, 2437);
					if (scenario == 28 && directed && directed_scans == 2) {   /* answers our probe in round 2 */
						std::vector<uint8_t> f = ap_beacon();
						f[0] = 0x50;
						memcpy(&f[4], kStaMac, 6);
						queue_ring_frame(f, (1u << 31) | (1u << 8), true, 52);
					}
					if (starts == 1 && scenario != 23 && scenario != 28) {
						queue_ring_frame(ap_beacon(), (1u << 31) | (1u << 8), true, 55);
						queue_ring_frame(ap_beacon(), (1u << 31) | (1u << 8), true, 57);
					} else if (starts == 1) {
						static const uint8_t other[6] = { 0x02, 0x77, 0, 0, 0, 7 };
						queue_ring_frame(make_beacon(other, "Neighbour", 6, true, 0x80), (1u << 31) | (1u << 8), true, 40);
					}
					if (starts > 1) ap_rescans++;
					queue_scan_event(2, 0, 0);
				} else if (scenario == 19) {
					queue_scan_event(0x40, 4, 0);
				} else if (scenario == 20 || scenario == 21) {
					static const uint8_t a[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01 }, b[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x02 };
					queue_scan_event(1, 0, 0);
					static const uint16_t mhz[37] = { 2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 2467, 2472,
						5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320, 5500, 5520, 5540, 5560, 5580, 5600, 5620, 5640, 5660,
						5680, 5700, 5745, 5765, 5785, 5805, 5825 };
					uint32_t cyc = 0xfff00000u, busy = 0xfffff000u, rx = 0x10u;   /* free-running, wrapping */
					for (uint32_t i = 0; i < 37; i++) {
						queue_scan_event(8, 0, mhz[i]);
						queue_chan_info(mhz[i], 0, -96 + (int32_t)(i % 4), busy, cyc, rx);
						cyc += 1000000; busy += scenario == 21 ? 0 : 250000; rx += (scenario == 20 && i == 5) ? 100000 : 0;
						if (scenario == 20 && i == 5) {                               /* 2437 MHz: ch 6 */
							for (int k = 0; k < 300; k++) {
								std::vector<uint8_t> f = k % 2 ? make_beacon(a, "RingNet", 6, true, 0x80) :
								    make_beacon(b, "NoDsIe", 0, false, 0x80);   /* channel from the scan */
								queue_ring_frame(f, (1u << 31) | (1u << 8), true, (uint8_t)(30 + k % 20));
							}
							std::vector<uint8_t> data = { 0x08, 0x02, 0, 0, 1, 2, 3, 4, 5, 6 };
							data.resize(60);
							queue_ring_frame(data, 1u << 31, true, 40);                              /* data: counted only */
							queue_ring_frame(make_beacon(a, "RingNet", 6, true, 0x80), (1u << 31) | (1u << 8), false, 55);  /* header only */
							queue_ring_frame(make_beacon(a, "RingNet", 6, true, 0x80), (1u << 31) | (1u << 30) | (1u << 8), true, 70); /* bad FCS */
						}
						queue_chan_info(mhz[i], 1, -96 + (int32_t)(i % 4), busy, cyc, rx);
					}
					queue_scan_event(2, 0, 0);
				} else {
					static const uint8_t a[6] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60 }, b[6] = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x61 },
					    c[6] = { 0x22, 0x22, 0x22, 0x22, 0x22, 0x22 };
					queue_scan_event(1, 0, 0);
					queue_scan_event(8, 0, 2412);
					queue_beacon(a, "HomeNet", 6, 40, true, 0, 0x80);
					queue_beacon(a, "HomeNet", 6, 50, true, 0, 0x50);       /* stronger, probe response */
					queue_beacon(c, "Cafe", 11, 20, false, 0, 0x80);
					queue_beacon(c, "Broken", 11, 60, false, 1, 0x80);     /* CRC error: dropped */
					queue_scan_event(8, 0, 5180);
					queue_beacon(b, "\0\0\0", 36, 30, true, 0, 0x80);       /* hidden */
					queue_scan_event(2, 0, 0);
				}
			}
			outq[1].push_back(htc(0, {}, credit_trailer(2, 1)));   /* the credit back */
			fw_credits++;
		} else if (ap_wmi(id, p, plen)) {
			outq[1].push_back(htc(0, {}, credit_trailer(2, 1)));   /* the credit back */
			fw_credits++;
		} else {
			fprintf(stderr, "SIM: unknown WMI command %x\n", id);
			abort();
		}
		return;
	}
	if (ce == 4) {                                            /* HTT */
		assert(eid == 1 && flags == 0 && plen >= 1);
		if (p[0] == 0) {                                     /* VERSION_REQ */
			assert(plen == 4 && p[1] == 0 && p[2] == 0 && p[3] == 0);
			version_reqs++;
			outq[1].push_back(htc(1, { 0, 44, 3, 0 }));      /* HTT 3.44, on pipe 1 (QCA6174 CE5 override) */
		} else if (p[0] == 2) {                              /* RX_RING_CFG, one 32-bit ring */
			assert(plen == 40 && p[1] == 1 && p[2] == 0 && p[3] == 0 && version_reqs == 1);
			uint32_t w[2]; memcpy(w, p + 4, 8);
			uint16_t h[14]; memcpy(h, p + 12, 28);
			ring_idx_pa = w[0]; ring_base = w[1]; ring_size = h[0];
			assert(h[1] == 2048 && h[2] == 0xffff);
			static const uint16_t off[10] = { 59, 75, 21, 31, 3, 20, 6, 10, 1, 2 };
			assert(memcmp(&h[4], off, sizeof(off)) == 0);
			uint32_t alloc; memcpy(&alloc, resolve(ring_idx_pa, 4), 4);
			assert(h[3] == alloc && alloc == 127 && ring_size == 256);   /* fw_idx_init_val = alloc_idx, filled first */
			for (uint32_t i = 0; i < alloc; i++) {
				uint32_t pa; memcpy(&pa, resolve(ring_base + 4 * i, 4), 4);
				uint8_t *d = resolve(pa, 2048);
				uint32_t at; memcpy(&at, d + 4, 4);
				assert(at == 0 && (pa & 3) == 0);
				for (uint32_t j = 0; j < i; j++) { uint32_t q; memcpy(&q, resolve(ring_base + 4 * j, 4), 4); assert(q != pa); }
			}
			ring_fw_rd = 0;
			ring_cfgs++;
		} else if (p[0] == 5) {                              /* AGGR_CFG */
			assert(plen == 3 && p[1] == 64 && p[2] == 3 && ring_cfgs == 1);
			aggr_cfgs++;
		} else {
			fprintf(stderr, "SIM: unknown HTT message %u\n", p[0]);
			abort();
		}
		return;
	}
	assert(!"message on an unexpected copy engine");
}

static void
chip_step(void)
{
	if (in_reset) {
		return;
	}
	ap_tick();
	if (!intr_at && (bar[0x3a008 / 4] & 0x400u)) {
		intr_at = now_us;
	}
	bar[0x3a028 / 4] = initialized() ? 2u : 0u;
	while (*reg(7, 0x44) != *reg(7, 0x3c)) {   /* CE7: host owns both rings */
		uint32_t si = *reg(7, 0x44), di = *reg(7, 0x48);
		if (di == *reg(7, 0x40)) {
			break;
		}
		uint32_t *s = desc(*reg(7, 0x00), si), *d = desc(*reg(7, 0x08), di);
		uint32_t n = s[1] & 0xffffu;
		memcpy(resolve(d[0], n), resolve(s[0], n), n);
		d[1] = (d[1] & 0xffff0000u) | n;
		*reg(7, 0x44) = (si + 1) % *reg(7, 0x04);
		*reg(7, 0x48) = (di + 1) % *reg(7, 0x0c);
	}
	while (initialized() && !bmi_done_at && *reg(0, 0x44) != *reg(0, 0x3c)) {   /* CE0 requests -> CE1 answers */
		uint32_t si = *reg(0, 0x44);
		uint32_t *sd = desc(*reg(0, 0x00), si);
		uint32_t n = sd[1] & 0xffffu, w[64];
		assert((sd[1] >> 16) == 0xfffcu && n >= 4 && n <= 256);
		memset(w, 0, sizeof(w));
		memcpy(w, resolve(sd[0], n), n);
		uint8_t rsp[256];
		uint32_t rlen = 0;
		switch (w[0]) {
		case 8: { uint32_t r3[3] = { 12u, 0x05030000u, 8u }; memcpy(rsp, r3, 12); rlen = 12; break; }
		case 2: assert(w[2] <= 256); memcpy(rsp, tmem(w[1], w[2]), w[2]); rlen = w[2]; break;
		case 3: assert(12 + w[2] == n); memcpy(tmem(w[1], w[2]), &w[3], w[2]); break;
		case 4: {
			exec_addr = w[1]; exec_param = w[2]; exec_params.push_back(w[2]);
			uint32_t r = w[2] == 0x10u ? 0x00001400u : 2u;   /* board id 5 / the "result 2" the phone shows */
			memcpy(rsp, &r, 4); rlen = 4; break;
		}
		case 6: {
			uint32_t v = soc[w[1]];
			if (w[1] == 0x1244u) { v = pll_changing > 0 ? 0x20u : 0u; if (pll_changing > 0) pll_changing--; }
			memcpy(rsp, &v, 4); rlen = 4; break;
		}
		case 7:
			assert(n == 12);
			soc[w[1]] = w[2];
			if (w[1] == 0x1014u) pll_changing = 3;
			break;
		case 5: app_start = w[1]; break;
		case 13: if (lz_on) { lz_last = lz_cur; lz_streams++; } lz_on = w[1] != 0; lz_cur.clear(); break;
		case 14: assert(lz_on && 8 + w[1] == n && (w[1] & 3) == 0); lz_cur.insert(lz_cur.end(), (uint8_t *)&w[2], (uint8_t *)&w[2] + w[1]); break;
		case 1: bmi_done_at = now_us; break;
		default: fprintf(stderr, "SIM: unknown BMI command %u\n", w[0]); abort();
		}
		if (rlen) {
			uint32_t di = *reg(1, 0x48);
			if (di == *reg(1, 0x40)) {
				break;   /* no response buffer posted: the target's send waits */
			}
			uint32_t *d = desc(*reg(1, 0x08), di);
			memcpy(resolve(d[0], rlen), rsp, rlen);
			d[1] = (d[1] & 0xffff0000u) | rlen;
			*reg(1, 0x48) = (di + 1) % *reg(1, 0x0c);
			answers++;
		}
		*reg(0, 0x44) = (si + 1) % *reg(0, 0x04);
	}
	if (bmi_done_at && !ready_sent && now_us >= bmi_done_at + 50000) {   /* the firmware has booted */
		if (scenario == 7) {
			crashed = true;
			ready_sent = true;
		} else if (scenario != 8) {
			/* as on the phone: 01 00 02 00 00 07 04 00 01 00 00 00 (2 credits of 1792 bytes, 4 endpoints, HTC 2.1) */
			if (deliver(1, htc(0, { 1, 0, (uint8_t)init_credits, 0, 0x00, 0x07, 4, 0, 1, 0, 0, 0 }))) {
				ready_sent = true;
			}
		}
	}
	if (ready_sent && !crashed) {
		for (unsigned ce : { 0u, 3u, 4u }) {   /* host -> target: the firmware reads every posted send */
			std::vector<uint8_t> gather;
			uint32_t gflags = 0;
			while (*reg(ce, 0x44) != *reg(ce, 0x3c)) {
				uint32_t si = *reg(ce, 0x44);
				uint32_t *sd = desc(*reg(ce, 0x00), si);
				uint32_t n = sd[1] & 0xffffu;
				const uint8_t *d = resolve(sd[0], n);
				if (gather.empty()) gflags = sd[1] >> 16;
				if ((sd[1] >> 16) & 1u) {                 /* CE_DESC_FLAGS_GATHER: more pieces follow */
					gather.insert(gather.end(), d, d + n);
				} else if (!gather.empty()) {
					gather.insert(gather.end(), d, d + n);
					fw_receive(ce, gather.data(), (uint32_t)gather.size(), gflags);
					gather.clear();
				} else {
					fw_receive(ce, d, n, sd[1] >> 16);
				}
				*reg(ce, 0x44) = (si + 1) % *reg(ce, 0x04);
			}
			assert(gather.empty());   /* the host moves the write index only after the last piece */
		}
		if (scenario == 16 && init_seen && !ready_queued_with_intr && (bar[0x3a008 / 4] & 0x400u)) {
			queue_ready(true);
			fw_credits++;
			ready_queued_with_intr = true;
		}
		if (credit_due_at && now_us >= credit_due_at) {   /* scenario 13: a trailer-only credit message on endpoint 0 */
			outq[1].push_back(htc(0, {}, credit_trailer(2, 1)));
			fw_credits++;
			credit_due_at = 0;
		}
		for (unsigned ce : { 1u, 2u }) {
			while (!outq[ce].empty()) {
				const std::vector<uint8_t> &m = outq[ce].front();
				if (!(ce == 2u && m[0] >= 0xFBu ? ring_put(m) : deliver(ce, m))) {
					break;
				}
				outq[ce].pop_front();
			}
		}
	}
	if (crashed) {
		bar[0x3a028 / 4] |= 1u;
	}
}

static std::string &logbuf = *new std::string;   /* everything the driver logged */
extern "C" void
IOLog(const char *fmt, ...)
{
	char line[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fputs(line, stdout);
	logbuf += line;
}
extern "C" void read_random(void *b, unsigned int n) { for (unsigned i = 0; i < n; i++) ((uint8_t *)b)[i] = (uint8_t)(0x5a ^ (i * 37)); }
extern "C" void IOSleep(unsigned ms) { now_us += ms * 1000ull; chip_step(); }
extern "C" void IODelay(unsigned us) { now_us += us; chip_step(); }
extern "C" void clock_get_uptime(uint64_t *r) { *r = now_us * 1000ull; }
extern "C" void absolutetime_to_nanoseconds(uint64_t a, uint64_t *r) { *r = a; }
extern "C" vm_offset_t
ml_io_map_wcomb(vm_offset_t phys, vm_size_t size)
{
	if (phys == kRxPhys) {
		assert(rxpool && size == rxpool_size);
		return (vm_offset_t)rxpool;
	}
	assert(phys == kDmaPhys && size == DMA_BYTES);
	return (vm_offset_t)dma;
}
extern "C" void *
IOMallocContiguous(vm_size_t size, vm_size_t align, IOPhysicalAddress *phys)
{
	assert(align == 4096);
	if (dma == NULL) {
		assert(size == DMA_BYTES);
		dma = (uint8_t *)aligned_alloc(4096, size);
		memset(dma, 0xa5, size);
		*phys = kDmaPhys;
		return dma;
	}
	if (size == 0x40000u) {   /* the HTT rx ring block */
		assert(rxpool == NULL);
		rxpool_size = size;
		rxpool = (uint8_t *)aligned_alloc(4096, size);
		memset(rxpool, 0xa5, size);
		*phys = kRxPhys;
		return rxpool;
	}
	uint64_t at = kChunkPhys;   /* host memory for the firmware */
	for (auto &c : chunks) at = c.phys + ((c.size + 4095) & ~(size_t)4095);
	chunk c = { at, size, (uint8_t *)aligned_alloc(4096, (size + 4095) & ~(size_t)4095) };
	memset(c.mem, 0xa5, size);
	chunks.push_back(c);
	*phys = at;
	return c.mem;
}

/* Stand-in for OP2Wifi.cpp's q_cold_reset. */
static bool
cold_reset(char *why, size_t n)
{
	cold_resets++;
	in_reset = true;
	IOSleep(20);
	if (scenario == 4) {
		snprintf(why, n, "in reset: link down, ep id ffffffff");
		return false;
	}
	if (scenario != 5) {
		for (unsigned ce = 0; ce < 8; ce++) {
			memset(&bar[CE(ce)], 0, 0x54);
		}
	}
	conf[0x04 / 4] = 0x00100002u;   /* command register back to memory-only, as the repair writes it */
	in_reset = false;
	released_at = now_us;
	IOSleep(20);
	return true;
}

static void st32(uint32_t off, uint32_t v);
static uint32_t t32(uint32_t off) { uint32_t v; memcpy(&v, tram + off, 4); return v; }
static void st32(uint32_t off, uint32_t v) { memcpy(tram + off, &v, 4); }

/* struct wmi_tlv_resource_config as in ath10k's wmi-tlv.h, field by field */
struct rc {
	uint32_t num_vdevs, num_peers, num_offload_peers, num_offload_reorder_bufs, num_peer_keys, num_tids, ast_skid_limit,
	    tx_chain_mask, rx_chain_mask, rx_timeout_pri[4], rx_decap_mode, scan_max_pending_reqs, bmiss_offload_max_vdev,
	    roam_offload_max_vdev, roam_offload_max_ap_profiles, num_mcast_groups, num_mcast_table_elems, mcast2ucast_mode,
	    tx_dbg_log_size, num_wds_entries, dma_burst_size, mac_aggr_delim, rx_skip_defrag_timeout_dup_detection_check,
	    vow_config, gtk_offload_max_vdev, num_msdu_desc, max_frag_entries, num_tdls_vdevs, num_tdls_conn_table_entries,
	    beacon_tx_offload_max_vdev, num_multicast_filter_entries, num_wow_filters, num_keep_alive_pattern,
	    keep_alive_pattern_size, max_tdls_concurrent_sleep_sta, max_tdls_concurrent_buffer_sta, wmi_send_separate,
	    num_ocb_vdevs, num_ocb_channels, num_ocb_schedules, host_capab;
};
static_assert(sizeof(struct rc) == 176, "resource config is 44 words");

int
main(int argc, char **argv)
{
	scenario = argc > 1 ? atoi(argv[1]) : 0;
	init_credits = fw_credits = scenario == 13 ? 1 : 2;
	tram = (uint8_t *)calloc(1, 0x100000);
	win = 0x7c5u << 21;                 /* as on the phone: 0xf8a00000 */
	bar[0x3a000 / 4] = 0x000487c5u;     /* CORE_CTRL */
	bar[0x80000 / 4] = 0x73;            /* RTC_STATE: awake */
	conf[0x04 / 4] = 0x00100002u;
	dbi[0x04 / 4] = 0x00100002u;
	conf[0x34 / 4] = 0x70;
	conf[0x70 / 4] = 0x00020010u;
	conf[0x80 / 4] = 0x10110000u;
	st32(0x8f8, 0x00401ee0u);
	st32(0x1ee0, 0x00404d90u);
	st32(0x1ee4, 0x00404e50u);
	st32(0x1f00, 0x1u);
	st32(0x8cc, 0u);
	st32(0x900, 0u);

	bar[0x3a018 / 4] = 4u;              /* CPU_ADDR_MSB: DRAM */
	st32(0x810, 0x42u);                 /* hi_option_flag: the firmware debug log disabled to start with */
	soc[0xb2cu] = 0x00000100u;          /* efuse: xtal_sel 1 = 19.2 MHz */
	soc[0xaf4u] = 0x00c00000u;          /* bb_pll_config: bits outside FRAC/OUTDIV must survive */
	soc[0x1018u] = 0x00001000u;         /* wlan_pll_settle */
	soc[0x914u] = 0x00000005u;          /* soc_core_clk_ctrl */
	soc[0x1014u] = 0x00010123u;         /* wlan_pll_control: bypassed, some old divider */
	soc[0x820u] = 0x00000003u;          /* soc_cpu_clock */
	st32(0x808, 0x00436000u);           /* hi_dbglog_hdr */
	st32(0x36000, 0x00436100u); st32(0x36004, 3u);
	st32(0x36100, 0x00436100u); st32(0x36104, 0x00436200u); st32(0x36108, 1500u); st32(0x3610c, 40u);
	st32(0x36110, 5u); st32(0x36114, 1460u);
	for (uint32_t i = 0; i < 10; i++) st32(0x36200 + 4 * i, 0xdb600000u + i);
	st32(0x854, 0x00430000u);           /* hi_board_data */
	st32(0x8ac, 0u);                    /* hi_board_ext_data */
	st32(0x804, 0x00435000u);           /* hi_failure_state */
	for (uint32_t i = 0; i < 60; i++) st32(0x35000 + 4 * i, 0xc0de0000u + i);
	struct op2_ath10k_bus bus = { bar, conf, dbi, 0, cold_reset, scenario >= 6, scenario >= 9, scenario >= 18, ap_scenario(),
		ap_scenario() && scenario != 27, scenario >= 31 && scenario <= 34, scenario == 33 || scenario == 34,
		scenario == 34 ? 61000u : 0u };
	PKCS5_PBKDF2_HMAC_SHA1(scenario == 25 ? "another-password" : "simulated-ap", scenario == 25 ? 16 : 12, (const unsigned char *)"iPhone", 6, 4096, 32, ap_pmk);
	fflush(stdout);
	char res[320];
	bool ok = op2_ath10k_step2b(&bus, res, sizeof(res));
	printf("RESULT ok=%d: %s\n", ok, res);

	int bad = 0;
#define CHECK(c) do { if (!(c)) { printf("CHECK FAILED: %s\n", #c); bad++; } } while (0)
	{
		static const uint32_t zero[0x54 / 4] = { 0 };
		CHECK(memcmp(&bar[CE(5)], zero, sizeof(zero)) == 0);   /* the host never touches CE5 */
	}
	CHECK((dbi[0x04 / 4] & 4u) == 0);
	if (scenario != 4) {
		CHECK((conf[0x04 / 4] & 4u) == 0 && (conf[0x04 / 4] & 0x400u));   /* BME off, INTx disable kept */
		CHECK(bar[0x80004 / 4] == 0);                                      /* wake released */
		CHECK(bar[0x3a008 / 4] == 0 && bar[0x3a014 / 4] == 0x7fc00u);      /* interrupt off and cleared */
	}
	switch (scenario) {
	case 0:
		CHECK(ok && strstr(res, "PASS (no reset)") && cold_resets == 0);
		/* ath10k_pci_override_ce_config: CE5 host->target 2048 bytes, HTT target->host on pipe 1 */
		CHECK(t32(0x4d90 + 5 * 24) == 5 && t32(0x4d90 + 5 * 24 + 4) == 2 && t32(0x4d90 + 5 * 24 + 8) == 32 &&
		    t32(0x4d90 + 5 * 24 + 12) == 2048);
		CHECK(t32(0x4e50 + 15 * 12) == 0x300 && t32(0x4e50 + 15 * 12 + 4) == 1 && t32(0x4e50 + 15 * 12 + 8) == 1);
		CHECK(t32(0x8cc) == 0x10u && t32(0x900) == 0x6d8a0009u && t32(0x1f00) == 0u);
		CHECK(memcmp(tram + 0x4d90, "\0\0\0\0\2\0\0\0\x20\0\0\0", 12) == 0);
		CHECK(t32(0x4e50) == 0x104 && t32(0x4e50 + 15 * 12) == 0x300 && t32(0x4e50 + 16 * 12) == 0);
		break;
	case 1:
		CHECK(ok && strstr(res, "PASS (no reset)") && intr_at != 0 && cold_resets == 0);
		break;
	case 2:
	case 5:
		CHECK(ok && strstr(res, "PASS (after cold reset)") && cold_resets == 1 && answers >= 1);
		break;
	case 3:
		CHECK(!ok && strstr(res, "(after cold reset)") && strstr(res, "no answer to BMI_GET_TARGET_INFO") && cold_resets == 1);
		break;
	case 4:
		CHECK(!ok && strstr(res, "cold reset failed") && bar[0x80004 / 4] == 1);   /* chip left alone */
		break;
	default: {
		CHECK(t32(0x800) == 2u && t32(0x810) == 0x220au && t32(0x844) == 0 && t32(0x904) == 0);
		/* the PLL switch for a 19.2 MHz crystal (qcacld ol_patch_pll_switch) */
		CHECK(soc[0xaf4u] == (0x00c00000u | 0x2aaa8u | (1u << 18)) && soc[0x1018u] == (0x1000u | 960u) && soc[0x914u] == 1u);
		CHECK(soc[0x1014u] == 0x24u && soc[0x820u] == 1u);
		CHECK(t32(0x4020) == 1u && t32(0x4024) == 176000000u && t32(0x4028) == 1u);
		CHECK(exec_params.size() == 2 && exec_params[0] == 0x10u && exec_params[1] == 0u);
		CHECK(t32(0x8bc) == 0x3eu && t32(0x858) == 1u && t32(0x814) == 0u);
		CHECK(memcmp(tram + 0x30000, kOp2WifiBoard, kOp2WifiBoardLen) == 0);           /* board data at hi_board_data */
		CHECK(exec_addr == 0x1234u && exec_param == 0u && lz_streams == 2);   /* the OTP image twice (board id run, then calibration) */
		/* OTP stream = the OTP IE, zero-padded to 4 bytes; firmware segments in target memory; start address */
		const uint8_t *fw = kOp2WifiFw;
		uint32_t off = 12, otp_off = 0, otp_len = 0, img_off = 0, img_len = 0;
		while (off + 8 <= kOp2WifiFwLen) {
			uint32_t id, l; memcpy(&id, fw + off, 4); memcpy(&l, fw + off + 4, 4);
			if (id == 4) { otp_off = off + 8; otp_len = l; }
			if (id == 3) { img_off = off + 8; img_len = l; }
			off += 8 + ((l + 3) & ~3u);
		}
		CHECK(lz_last.size() == ((otp_len + 3) & ~3u) && memcmp(lz_last.data(), fw + otp_off, otp_len) == 0);
		uint32_t p = img_off + 8, segs = 0;
		while (p + 8 <= img_off + img_len) {
			uint32_t a, l; memcpy(&a, fw + p, 4); memcpy(&l, fw + p + 4, 4); p += 8;
			if (l == 0xffffffffu) break;
			if (l == 0xfffffffdu) { CHECK(app_start == a); continue; }
			for (uint32_t done = 0; done < l; ) {   /* compare per 1 MB region */
				uint32_t aa = a + done, chunk = 0x100000u - (aa & 0xfffffu);
				if (chunk > l - done) chunk = l - done;
				if (memcmp(tmem(aa, chunk), fw + p + done, chunk) != 0) { printf("segment %08x+%u differs\n", a, done); bad++; break; }
				done += chunk;
			}
			p += l; segs++;
		}
		CHECK(segs == 6 && bar[0x3a018 / 4] == 4u);
		break;
	}
	}
	switch (scenario) {
	case 6: CHECK(ok && strstr(res, "STEP 2c PASS") && strstr(res, "2 credits of 1792 bytes, 4 endpoints, extended")); break;
	case 7: CHECK(!ok && strstr(res, "firmware crashed")); break;
	case 8: CHECK(!ok && strstr(res, "no HTC ready")); break;
	case 9: case 10: case 13: case 16: case 18: case 19: case 20: case 21: {
		if (scenario >= 18) {
			CHECK(regd_rd == 0x6a && chanlist_n == 37 && chanlist_passive == 37 && vdev_creates == 2 && vdev_deletes == 1);
			CHECK(memcmp(vdev_mac, "\x00\x03\x7f\x12\x34\x56", 6) == 0 && starts == 1 && scan_flags == 0x31 && scan_nchan == 37);
			CHECK(regd_seen && ring_cfg_before_regd && ring_cfgs == 1 && aggr_cfgs == 1 && params_before_regd);
			/* ath10k_start's order; the simulated board has 2 RF chains: mask 3; no burst service */
			static const std::pair<uint32_t, uint32_t> want[] = { { 0x22, 1 }, { 0x0a, 1 }, { 0x2c, 1 }, { 0x01, 3 }, { 0x02, 3 },
				{ 0x23, 0 }, { 0x25, 1 } };
			CHECK(pdev_params.size() == 7 && std::equal(pdev_params.begin(), pdev_params.end(), want));
			if (scenario == 18) {
				CHECK(ok && strstr(res, "STEP 4 PASS: listen-only scan found 3 networks") && strstr(res, "strongest: HomeNet, ch 6, -45 dBm, WPA2"));
				CHECK(strstr(res, "(beacons: 4 over WMI, 0 from the rx ring)") && ring_written == 0);
			} else if (scenario == 20) {
				/* 300 + 1 header-only beacons parsed; the strongest RingNet frame: rssi 70 had a bad FCS, so 55 - 95 */
				CHECK(ok && strstr(res, "STEP 4 PASS: listen-only scan found 2 networks") && ring_written == 303);
				CHECK(strstr(res, "strongest: RingNet, ch 6, -40 dBm, WPA2 (beacons: 0 over WMI, 301 from the rx ring)"));
				CHECK(logbuf.find("NoDsIe                           ch   6  -47 dBm open") != std::string::npos);
				CHECK(logbuf.find("rx ring: 303 frames (301 management, 0 control, 1 bad FCS, 301 beacons parsed)") != std::string::npos);
				CHECK(logbuf.find("receiving on 1\n") != std::string::npos);
			} else if (scenario == 21) {
				CHECK(!ok && strstr(res, "STEP 4 FAIL: scan completed") && strstr(res, "WMI 0 frames, rx ring 0 frames, 0 HTT rx "
				    "indications; noise floor -96..-93 dBm, receiving on 0 of 37 channels"));
			} else {
				CHECK(!ok && strstr(res, "STEP 4 FAIL: scan start failed"));
			}
			break;
		}
		CHECK(ok && strstr(res, "STEP 3 PASS: firmware ready: MAC 00:03:7f:12:34:56, HTT 3.44, 2 RF chains"));
		CHECK(strstr(res, "2 GHz 2312-2732 MHz, 5 GHz 4912-6100 MHz, regdomain 6a, WMI echo OK"));
		CHECK(connects == 2 && setup_completes == 1 && inits == 1 && version_reqs == 1 && echos == 1);
		CHECK(scenario != 16 || ready_queued_with_intr);
		CHECK(echo_value == 0x4F503221u && seq_seen[0] == 3 && seq_seen[1] == 1 && seq_seen[2] == 2);
		/* the init command, TLV by TLV */
		const uint8_t *c = init_cmd.data();
		uint32_t w[9]; memcpy(w, c, sizeof(w));
		CHECK(w[0] == (28u | (74u << 16)) && w[1] == 0x01000000u && w[2] == 53u && w[3] == 0x5F414351u && w[4] == 0x4C4Du &&
		    w[5] == 0 && w[6] == 0);
		uint32_t nchunks = w[7];
		CHECK(w[8] == (176u | (75u << 16)));
		struct rc r; memcpy(&r, c + 36, sizeof(r));
		CHECK(r.num_vdevs == 4 && r.num_peers == 33 && r.num_offload_peers == 4 && r.num_offload_reorder_bufs == 4);
		CHECK(r.num_peer_keys == 2 && r.num_tids == 66 && r.ast_skid_limit == 0x10 && r.tx_chain_mask == 7 && r.rx_chain_mask == 7);
		CHECK(r.rx_timeout_pri[0] == 0x64 && r.rx_timeout_pri[1] == 0x64 && r.rx_timeout_pri[2] == 0x64 && r.rx_timeout_pri[3] == 0x28);
		CHECK(r.rx_decap_mode == 1 && r.scan_max_pending_reqs == 4 && r.bmiss_offload_max_vdev == 4 && r.roam_offload_max_vdev == 4);
		CHECK(r.roam_offload_max_ap_profiles == 8 && r.num_mcast_groups == 0 && r.num_mcast_table_elems == 0 && r.mcast2ucast_mode == 0);
		CHECK(r.tx_dbg_log_size == 0x400 && r.num_wds_entries == 0x20 && r.dma_burst_size == 0 && r.mac_aggr_delim == 0);
		CHECK(r.rx_skip_defrag_timeout_dup_detection_check == 0 && r.vow_config == 0 && r.gtk_offload_max_vdev == 2);
		CHECK(r.num_msdu_desc == 1056 && r.max_frag_entries == 2 && r.num_tdls_vdevs == 1 && r.num_tdls_conn_table_entries == 0x20);
		CHECK(r.beacon_tx_offload_max_vdev == 2 && r.num_multicast_filter_entries == 5 && r.num_wow_filters == 22);
		CHECK(r.num_keep_alive_pattern == 6 && r.keep_alive_pattern_size == 0 && r.max_tdls_concurrent_sleep_sta == 1);
		CHECK(r.max_tdls_concurrent_buffer_sta == 1 && r.wmi_send_separate == 0 && r.num_ocb_vdevs == 0);
		CHECK(r.num_ocb_channels == 0 && r.num_ocb_schedules == 0 && r.host_capab == 0x200);
		uint32_t ah; memcpy(&ah, c + 36 + 176, 4);
		CHECK(ah == ((nchunks * 20u) | (18u << 16)) && init_cmd.size() == 36 + 176 + 4 + nchunks * 20u);
		if (scenario == 10) {
			CHECK(nchunks == 2 && chunks.size() == 2);
			uint32_t k[10]; memcpy(k, c + 36 + 176 + 4, sizeof(k));
			CHECK(k[0] == (16u | (76u << 16)) && k[1] == 1 && k[2] == (uint32_t)chunks[0].phys && k[3] == 34u * 2216u && k[4] == 0);
			CHECK(k[5] == (16u | (76u << 16)) && k[6] == 2 && k[7] == (uint32_t)chunks[1].phys && k[8] == 12u * 256u && k[9] == 0);
			CHECK(chunks[0].size == 34u * 2216u && chunks[1].size == 3072u);
			bool zero = true;
			for (auto &ch : chunks) for (size_t i = 0; i < ch.size; i++) zero = zero && ch.mem[i] == 0;
			CHECK(zero);
		} else {
			CHECK(nchunks == 0 && chunks.empty());
		}
		break;
	}
	case 22: case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31: case 32: case 33: case 34: {
		CHECK(regd_seen && ring_cfg_before_regd && pdev_params.size() == 7);
		CHECK(ap_rescans == (scenario == 23 ? 5u : scenario == 28 ? 2u : 0u));
		CHECK(directed_scans == (scenario == 23 ? 4u : scenario == 28 ? 2u : 0u));
		CHECK(chanlists == (scenario == 23 || scenario == 28 ? 2u : 1u));
		bool joined = scenario == 22 || scenario == 24 || scenario == 25 || scenario >= 27;
		CHECK(ap_vdev_starts == (joined ? 1u : 0u) && ap_peer_creates == (joined ? 1u : 0u) && ap_auths == (joined ? 1u : 0u));
		/* leaving: vdev stop always once started; deauth, vdev down and peer delete only for what was set up */
		bool assoc = scenario == 22 || scenario == 25 || scenario >= 27;
		CHECK(ap_vdev_stops == (joined ? 1u : 0u) && ap_peer_deletes == (joined ? 1u : 0u) && ap_order_ok);
		CHECK(ap_vdev_downs == (assoc ? 1u : 0u) && ap_vdev_ups == (assoc ? 1u : 0u) && ap_peer_assocs == (assoc ? 1u : 0u));
		CHECK(ap_deauths == ((scenario == 22 || (scenario >= 27 && scenario != 33)) ? 1u : 0u) && (ap_deauths == 0 || ap_deauth_reason == 3));
		CHECK(!assoc || ap_pa_ok);
		CHECK(logbuf.find("left the network: vdev down, peer delete, vdev stop confirmed") != std::string::npos || !joined ||
		    !assoc);
		char tk[33];
		for (int i = 0; i < 16; i++) snprintf(tk + 2 * i, 3, "%02x", ap_ptk[32 + i]);
		CHECK(logbuf.find(tk) == std::string::npos);   /* no key material in the log */
		switch (scenario) {
		case 22:
			CHECK(ok && strstr(res, "STEP 6 PASS: WPA2 connected to iPhone: 4-way handshake in"));
			CHECK(strstr(res, "(message 1 seen 1 time(s)), pairwise and group key 1 installed (firmware confirmed 2 of 2), port "
			    "open; 3 data frames from the network in 5 s, no deauthentication"));
			CHECK(logbuf.find("STEP 5 PASS: associated with iPhone (BSSID 02:1a:11:f0:00:01, channel 6, -38 dBm), AID 1, "
			    "WPA2-PSK CCMP, 12 rates (11g); ") != std::string::npos);
			CHECK(ap_msg1s == 1 && ap_msg2_ok == 1 && ap_msg4_ok == 1 && ap_keys_ok == 2 && ap_keys_bad == 0 && ap_authorized == 1);
			CHECK(ap_txfrm == 5);   /* auth, assoc, message 2, message 4, deauth */
			break;
		case 23: CHECK(!ok && strstr(res, "STEP 5 FAIL: joining iPhone: not heard in 2 listen-only scans, and no answer to "
		    "probe requests on 2.4 GHz (4 rounds)")); break;
		case 31:
			CHECK(ok && strstr(res, "STEP 7 PASS: data through WPA2 with iPhone: DHCP lease 172.20.10.2/28 (router 172.20.10.1, DNS "
			    "172.20.10.1, 86400 s) in "));
			CHECK(strstr(res, "ping 172.20.10.1: 3 of 3 replies") && strstr(res, "1 ARP requests for us answered"));
			CHECK(ap_discovers == 1 && ap_requests == 1 && ap_arp_from_sta == 1 && ap_arp_reply_from_sta == 1 && ap_pings == 3 &&
			    ap_bad_csum == 0);
			CHECK(logbuf.find("DHCP offer: 172.20.10.2 from 172.20.10.1") != std::string::npos);
			break;
		case 33:
		case 34:
			CHECK(ok && strstr(res, "STEP 8 PASS: wl0 up "));
			CHECK(logbuf.find("STEP 7 PASS: data through WPA2 with iPhone") != std::string::npos);
			CHECK(logbuf.find("STEP 8 PASS: wl0 up with 172.20.10.2/28 (router 172.20.10.1, DNS 172.20.10.1)") != std::string::npos);
			CHECK(st.attached && st.up && st.down && memcmp(st.mac, kStaMac, 6) == 0 && st.ip == kStaIp && st.mask == 0xfffffff0u);
			CHECK(st.router == kApIp && st.dns == kApIp && st.router_known && memcmp(st.router_mac, kApMac, 6) == 0);
			CHECK(st_own == 0 && st_bad == 0 && st_out.empty() && ap_bad_csum == 0 && ap_pend.empty());
			if (scenario == 33) {
				/* to iOS: ARP reply, 20 echo replies, the AP's ARP request, IPv6 (our repeated broadcast not); from iOS: ARP request,
				 * 20 pings, the last ping, the ARP reply */
				CHECK(strstr(res, ": 23 frames (28 KB) to iOS, 23 frames (28 KB) from iOS (23 acknowledged, 0 not, 0 discarded, 0 unconfirmed; 0 dropped"));
				CHECK(strstr(res, "then the network deauthenticated us (reason 4, inactivity)"));
				CHECK(st_echo_replies == 20 && st_ipv6 == 1 && st_arp_answered == 1 && ap_pings == 3 + 21);
				CHECK(ap_arp_from_sta == 2 && ap_arp_reply_from_sta == 2 && ap_pend_max >= 4 && ap_multi_compl >= 5);
			} else {
				CHECK(strstr(res, "STEP 8 PASS: wl0 up 61 s: 2 frames (0 KB) to iOS, 2 frames (0 KB) from iOS (2 acknowledged") && strstr(res, "1 dropped"));
				CHECK(strstr(res, "then the time set for this run ended") && st_echo_replies == 1 && ap_pings == 4);
				CHECK(logbuf.find("8: wl0 up 60 s: ") != std::string::npos);
			}
			break;
		case 32:
			CHECK(!ok && strstr(res, "STEP 7 FAIL: data with iPhone: no DHCP offer to 4 discovers") && ap_discovers == 4);
			CHECK(logbuf.find("STEP 6 PASS") != std::string::npos);
			break;
		case 30:
			CHECK(ok && strstr(res, "STEP 6 PASS: WPA2 connected to iPhone") && ap_keys_ok == 2 && ap_authorized == 1);
			CHECK(logbuf.find("message 4: sent, but the firmware never confirmed it (2 s)") != std::string::npos);
			CHECK(logbuf.find("firmware tx pause: id 6 (station add BA), stop, vdev map 1, peer 76, tid map 1 (not a vdev "
			    "pause: ignored, as ath10k does)") != std::string::npos);
			CHECK(logbuf.find("firmware tx pause: id 1 (MCC), wake, vdev map 1, peer 76, tid map 0") != std::string::npos);
			CHECK(logbuf.find("tx still paused") == std::string::npos);
			break;
		case 29:
			CHECK(ok && strstr(res, "STEP 6 PASS: WPA2 connected to iPhone") && ap_keys_ok == 2 && ap_authorized == 1);
			CHECK(strstr(res, "3 data frames from the network in 5 s") && stuck_buffers == 1);
			CHECK(logbuf.find("HTT in-order rx indication: info 20 (tid 0, offloaded), peer 150 vdev 0, 1 buffers") !=
			    std::string::npos);
			break;
		case 28:
			CHECK(ok && strstr(res, "STEP 6 PASS: WPA2 connected to iPhone") && ap_keys_ok == 2 && ap_authorized == 1);
			CHECK(logbuf.find("heard 1 time(s), answered our probe requests") != std::string::npos);
			CHECK(logbuf.find("probe round 1: not heard") != std::string::npos);
			CHECK(logbuf.find("probe round 2: found") != std::string::npos);
			break;
		case 24: CHECK(!ok && strstr(res, "STEP 5 FAIL: joining iPhone: association refused, status 17 (access point is full)")); break;
		case 25:
			CHECK(!ok && strstr(res, "STEP 6 FAIL: WPA2 with iPhone: it sent a deauthentication, reason 15 (4-way handshake "
			    "timeout) after 3 message 1(s): the password is probably wrong"));
			CHECK(ap_msg2_bad == 3 && ap_msg2_ok == 0 && ap_keys_ok == 0);
			break;
		case 26: CHECK(!ok && strstr(res, "STEP 5 FAIL: joining iPhone: it is WPA3-only (SAE); turn on Maximize Compatibility")); break;
		case 27: CHECK(ok && strstr(res, "STEP 5 PASS: associated with iPhone") && ap_msg1s == 0); break;
		}
		break;
	}
	case 11: CHECK(!ok && strstr(res, "STEP 3 FAIL: no WMI ready within 16 s") && strstr(res, "WMI echo OK") && inits == 1 &&
	    echos == 1); break;
	case 12: CHECK(!ok && strstr(res, "STEP 3 FAIL: firmware crashed while we waited for the WMI ready event")); break;
	case 14: CHECK(!ok && strstr(res, "STEP 3 FAIL: HTC connect of HTT data refused") && connects == 1 && !init_seen); break;
	case 15: CHECK(!ok && strstr(res, "STEP 3 FAIL: firmware WMI ABI") && !init_seen); break;
	case 17: CHECK(!ok && strstr(res, "no WMI ready within 16 s") && strstr(res, "firmware crash record written")); break;
	}
	printf("%s (%d failed checks), simulated %llu ms\n", bad ? "SIM FAIL" : "SIM OK", bad, (unsigned long long)(now_us / 1000));
	return bad ? 1 : 0;
}
