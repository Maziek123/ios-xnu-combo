/* ---- access point model for steps 5 and 6 (scenarios 22-27), crypto by OpenSSL ----
 * 22: WPA2-PSK "iPhone", password simulated-ap: join, 4-way handshake, keys, 3 data frames after authorize, leave
 * 23: the network is never heard (another one is): 3 scans, then STEP 5 FAIL
 * 24: the access point refuses the association (status 17)
 * 25: the access point has another password: message 2's MIC fails, message 1 three times, deauth reason 15
 * 26: the access point is WPA3-only (SAE): STEP 5 FAIL before anything is sent
 * 27: wifi=8: join only (STEP 5 PASS), then leave
 * 29: as 22, but in rx full reorder mode as the phone's firmware does it: right after the association the chip takes a
 *     buffer it never finishes, message 1 and the data frames come offloaded, message 3 with a descriptor, each
 *     announced by an in-order indication
 * 30: as 22, but as on the phone: tx pause events come with message 3 (station add BA stop/wake, ignored; MCC
 *     stop/wake for vdev 0) and the firmware never confirms message 4
 * 31: wifi=10: as 22, then data through the keys: DHCP (offer 172.20.10.2/28, router and DNS .1), our ARP for the
 *     router answered, the hotspot ARPs for us (we must answer), 3 pings answered
 * 32: wifi=10, the DHCP server stays silent: STEP 7 FAIL after 4 discovers
 * 33: wifi=11: as 31, then step 8: wl0 for a stand-in of iOS's stack (harness.cpp): its ARP for the router, 20 pings of
 *     1400 bytes at once (more than the 16 transmit slots; the AP confirms them 4 at a time in one TX_COMPL_IND), the
 *     hotspot's ARP for us answered by the stack, an IPv6 frame and our own broadcast repeated by the AP; after a last
 *     ping the AP deauthenticates us (reason 4): STEP 8 PASS, ended by the network
 * 34: wifi=11 for 61 s (iface_ms): one ping, a frame with an 802.3 length field (dropped), then quiet; the 60 s status
 *     line, then the time runs out and we leave (deauthentication reason 3)
 * 35: wifi=12, the Wi-Fi manager: the build's network joined first (and pinged by the stack stand-in), then op2netd's
 *     commands (harness.cpp): scan while connected, leave, join with a wrong PMK (message 2's MIC fails, deauth 15:
 *     "failed"), join with the right one (connected again, a new lease and ping), off. Every scan hears the hotspot and
 *     an open "Cafe"; the hotspot starts a fresh session at each vdev stop.
 * 36: wifi=12, the build's network not around (only "Cafe"): skipped at once (no probing), a periodic rescan while
 *     idle, then off
 * 28: the hotspot never beacons where we listen but answers our probe request in the second directed round: WPA2 as 22 */
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/aes.h>

static const uint8_t kApMac[6] = { 0x02, 0x1a, 0x11, 0xf0, 0x00, 0x01 };
static const uint8_t kStaMac[6] = { 0x00, 0x03, 0x7f, 0x12, 0x34, 0x56 };
static const uint8_t kBcast_sim[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t kGtk[16] = { 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f };
static uint8_t ap_pmk[32], ap_ptk[48], ap_anonce[32], ap_snonce[32];
static uint64_t ap_replay;
static uint32_t ap_auths, ap_assocs, ap_msg1s, ap_msg2_ok, ap_msg2_bad, ap_msg4_ok, ap_deauths, ap_deauth_reason;
static uint32_t ap_ps_off, ap_vdev_starts, ap_peer_creates, ap_peer_assocs, ap_vdev_ups, ap_keys_ok, ap_keys_bad,
    ap_authorized, ap_vdev_downs, ap_peer_deletes, ap_vdev_stops, ap_txfrm, ap_rescans;
static bool ap_pa_ok, ap_order_ok = true;
static uint32_t ap_state;   /* 0 idle, 1 authenticated, 2 associated, 3 msg3 sent, 4 done */

static bool ap_scenario(void) { return scenario >= 22 && scenario <= 36; }
static uint32_t ap_sessions, ap_authorized_total;

static std::vector<uint8_t>
ap_rsn(void)
{
	uint8_t akm = scenario == 26 ? 8 : 2;
	return { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, akm, 0x80, 0 };   /* MFPC */
}

static std::vector<uint8_t>
ap_beacon(void)
{
	std::vector<uint8_t> f = { 0x80, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	f.insert(f.end(), kApMac, kApMac + 6);
	f.insert(f.end(), kApMac, kApMac + 6);
	f.insert(f.end(), { 0x10, 0x00, 1, 2, 3, 4, 5, 6, 7, 8, 0x64, 0, 0x31, 0x04 });   /* ESS, privacy, short preamble, short slot */
	f.insert(f.end(), { 0, 6, 'i', 'P', 'h', 'o', 'n', 'e' });
	f.insert(f.end(), { 1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 });
	f.insert(f.end(), { 3, 1, 6 });
	f.insert(f.end(), { 45, 2, 0x2c, 0x01 });   /* HT capabilities (ignored by a legacy station) */
	f.insert(f.end(), { 50, 4, 0x30, 0x48, 0x60, 0x6c });
	std::vector<uint8_t> r = ap_rsn();
	f.insert(f.end(), r.begin(), r.end());
	f.insert(f.end(), { 221, 7, 0x00, 0x50, 0xf2, 2, 0, 1, 0 });   /* WMM information */
	return f;
}

static void
ap_hmac(const uint8_t *key, size_t kl, const std::vector<uint8_t> &d, uint8_t out[20])
{
	unsigned int ol = 20;
	HMAC(EVP_sha1(), key, (int)kl, d.data(), d.size(), out, &ol);
}

static void
ap_prf(const uint8_t *key, const char *label, const std::vector<uint8_t> &data, uint8_t *out, size_t n)
{
	for (uint8_t i = 0; n > 0; i++) {
		std::vector<uint8_t> m(label, label + strlen(label));
		m.push_back(0);
		m.insert(m.end(), data.begin(), data.end());
		m.push_back(i);
		uint8_t h[20];
		ap_hmac(key, 32, m, h);
		size_t take = n < 20 ? n : 20;
		memcpy(out, h, take);
		out += take;
		n -= take;
	}
}

static void
ap_derive_ptk(void)
{
	std::vector<uint8_t> d;
	bool ml = memcmp(kApMac, kStaMac, 6) < 0, nl = memcmp(ap_anonce, ap_snonce, 32) < 0;
	d.insert(d.end(), ml ? kApMac : kStaMac, (ml ? kApMac : kStaMac) + 6);
	d.insert(d.end(), ml ? kStaMac : kApMac, (ml ? kStaMac : kApMac) + 6);
	d.insert(d.end(), nl ? ap_anonce : ap_snonce, (nl ? ap_anonce : ap_snonce) + 32);
	d.insert(d.end(), nl ? ap_snonce : ap_anonce, (nl ? ap_snonce : ap_anonce) + 32);
	ap_prf(ap_pmk, "Pairwise key expansion", d, ap_ptk, 48);
}

/* an EAPOL-Key frame from the AP in a native wifi data frame (from DS) into the rx ring */
static void
ap_send_ek(uint16_t ki, const std::vector<uint8_t> &kd, bool mic)
{
	std::vector<uint8_t> e(99, 0);
	e[0] = 2; e[1] = 3;
	size_t body = 95 + kd.size();
	e[2] = (uint8_t)(body >> 8); e[3] = (uint8_t)body;
	e[4] = 2; e[5] = (uint8_t)(ki >> 8); e[6] = (uint8_t)ki; e[8] = 16;
	for (int i = 0; i < 8; i++) e[9 + i] = (uint8_t)(ap_replay >> (56 - 8 * i));
	memcpy(&e[17], ap_anonce, 32);
	e[97] = (uint8_t)(kd.size() >> 8); e[98] = (uint8_t)kd.size();
	e.insert(e.end(), kd.begin(), kd.end());
	if (mic) {
		uint8_t h[20];
		ap_hmac(ap_ptk, 16, e, h);
		memcpy(&e[81], h, 16);
	}
	std::vector<uint8_t> f = { 0x08, 0x02, 0, 0 };
	f.insert(f.end(), kStaMac, kStaMac + 6);
	f.insert(f.end(), kApMac, kApMac + 6);
	f.insert(f.end(), kApMac, kApMac + 6);
	f.insert(f.end(), { 0, 0, 0xaa, 0xaa, 0x03, 0, 0, 0, 0x88, 0x8e });
	f.insert(f.end(), e.begin(), e.end());
	queue_ring_frame(f, 1u << 31, true, 50, scenario != 29 ? 0xFE : (ki & 0x0100) ? 0xFD : 0xFC);
}

static void
ap_msg1(void)
{
	ap_replay++;
	ap_msg1s++;
	ap_send_ek(0x008a, {}, false);
}

static void
ap_mgmt_to_sta(uint8_t fc0, const std::vector<uint8_t> &body, bool via_ring)
{
	std::vector<uint8_t> f = { fc0, 0, 0, 0 };
	f.insert(f.end(), kStaMac, kStaMac + 6);
	f.insert(f.end(), kApMac, kApMac + 6);
	f.insert(f.end(), kApMac, kApMac + 6);
	f.insert(f.end(), { 0, 0 });
	f.insert(f.end(), body.begin(), body.end());
	if (via_ring) {
		queue_ring_frame(f, (1u << 31) | (1u << 8), true, 60);
		return;
	}
	uint32_t blen = (uint32_t)f.size();
	while (f.size() % 4) f.push_back(0);
	std::vector<uint8_t> p;
	put32(p, 0x7001u);
	tlv(p, 44, { 6, 50, 0, 0, blen, 0, 0, 0, 0, 0 });
	put32(p, (uint32_t)f.size() | (17u << 16));
	p.insert(p.end(), f.begin(), f.end());
	outq[2].push_back(htc(2, p));
}

/* ---- the hotspot's IP side (scenarios 31, 32): DHCP server, ARP, ping ---- */
static const uint32_t kApIp = 0xac140a01u, kStaIp = 0xac140a02u;   /* 172.20.10.1, 172.20.10.2 */
static uint32_t ap_discovers, ap_requests, ap_arp_from_sta, ap_arp_reply_from_sta, ap_pings, ap_bad_csum;

static uint16_t
ap_csum(const uint8_t *p, size_t n)
{
	uint32_t s = 0;
	for (size_t i = 0; i < n; i += 2) s += (uint32_t)(p[i] << 8) | (i + 1 < n ? p[i + 1] : 0);
	while (s >> 16) s = (s & 0xffff) + (s >> 16);
	return (uint16_t)~s;
}

static void be32put(std::vector<uint8_t> &v, uint32_t x) { for (int i = 3; i >= 0; i--) v.push_back((uint8_t)(x >> (8 * i))); }
static void be16put(std::vector<uint8_t> &v, uint32_t x) { v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }

/* to the station: native wifi from DS (the chip has decrypted it; Protected left set), LLC/SNAP, as an offloaded
 * in-order buffer like the phone's firmware delivers them */
static void
ap_to_sta(const uint8_t da[6], uint16_t ethertype, const std::vector<uint8_t> &payload, const uint8_t *sa = kApMac)
{
	std::vector<uint8_t> f = { 0x08, 0x42, 0, 0 };
	f.insert(f.end(), da, da + 6);
	f.insert(f.end(), kApMac, kApMac + 6);
	f.insert(f.end(), sa, sa + 6);
	f.insert(f.end(), { 0, 0, 0xaa, 0xaa, 0x03, 0, 0, 0 });
	be16put(f, ethertype);
	f.insert(f.end(), payload.begin(), payload.end());
	queue_ring_frame(f, 1u << 31, true, 50, 0xFC);
}

static std::vector<uint8_t>
ap_ipv4(uint8_t proto, uint32_t src, uint32_t dst, const std::vector<uint8_t> &l4)
{
	std::vector<uint8_t> p = { 0x45, 0 };
	be16put(p, (uint32_t)(20 + l4.size()));
	p.insert(p.end(), { 0, 1, 0, 0, 64, proto, 0, 0 });
	be32put(p, src);
	be32put(p, dst);
	uint16_t c = ap_csum(p.data(), 20);
	p[10] = (uint8_t)(c >> 8); p[11] = (uint8_t)c;
	p.insert(p.end(), l4.begin(), l4.end());
	return p;
}

static void
ap_dhcp_reply(const uint8_t *req, uint8_t type)
{
	std::vector<uint8_t> b(240, 0);
	b[0] = 2; b[1] = 1; b[2] = 6;
	memcpy(&b[4], req + 4, 4);          /* xid */
	b[10] = 0x80;
	b[16] = 0xac; b[17] = 0x14; b[18] = 0x0a; b[19] = 0x02;   /* yiaddr 172.20.10.2 */
	memcpy(&b[28], req + 28, 16);       /* chaddr */
	b[236] = 0x63; b[237] = 0x82; b[238] = 0x53; b[239] = 0x63;
	b.insert(b.end(), { 53, 1, type, 54, 4, 0xac, 0x14, 0x0a, 0x01, 1, 4, 0xff, 0xff, 0xff, 0xf0, 3, 4, 0xac, 0x14, 0x0a, 0x01,
		6, 4, 0xac, 0x14, 0x0a, 0x01, 51, 4, 0, 1, 0x51, 0x80, 255 });   /* lease 86400 s */
	std::vector<uint8_t> u;
	be16put(u, 67); be16put(u, 68); be16put(u, (uint32_t)(8 + b.size())); be16put(u, 0);
	u.insert(u.end(), b.begin(), b.end());
	ap_to_sta(kBcast_sim, 0x0800, ap_ipv4(17, kApIp, 0xffffffffu, u));
}

/* an IP-side payload from the station (dst: the 802.11 DA) */
static void
ap_ip(const uint8_t *dst, uint32_t ethertype, const uint8_t *p, uint32_t n)
{
	if (ethertype == 0x0806) {                         /* ARP */
		assert(n == 28 && p[0] == 0 && p[1] == 1 && p[2] == 8 && p[3] == 0 && p[4] == 6 && p[5] == 4 && memcmp(p + 8, kStaMac, 6) == 0);
		if (p[7] == 1) {                               /* who has 172.20.10.1 */
			assert(memcmp(dst, kBcast_sim, 6) == 0 && ((uint32_t)p[24] << 24 | p[25] << 16 | p[26] << 8 | p[27]) == kApIp);
			ap_arp_from_sta++;
			std::vector<uint8_t> r = { 0, 1, 8, 0, 6, 4, 0, 2 };
			r.insert(r.end(), kApMac, kApMac + 6); be32put(r, kApIp);
			r.insert(r.end(), kStaMac, kStaMac + 6); be32put(r, kStaIp);
			ap_to_sta(kStaMac, 0x0806, r);
		} else {                                       /* the station's answer to our ARP request */
			assert(p[7] == 2 && memcmp(dst, kApMac, 6) == 0 && memcmp(p + 18, kApMac, 6) == 0);
			ap_arp_reply_from_sta++;
		}
		return;
	}
	assert(ethertype == 0x0800 && n >= 28 && p[0] == 0x45);
	if (ap_csum(p, 20) != 0) ap_bad_csum++;
	if (p[9] == 17) {                                  /* DHCP */
		assert(p[20] == 0 && p[21] == 68 && p[22] == 0 && p[23] == 67 && memcmp(dst, kBcast_sim, 6) == 0);
		const uint8_t *b = p + 28;
		assert(n >= 28 + 240 && b[0] == 1 && memcmp(b + 28, kStaMac, 6) == 0 && b[236] == 0x63 && b[240] == 53 && b[241] == 1);
		uint8_t type = b[242];
		if (type == 1) {
			ap_discovers++;
			if (scenario != 32) ap_dhcp_reply(b, 2);
		} else if (type == 3) {
			ap_requests++;
			/* requested address 172.20.10.2 and our server id in options 50 and 54 */
			bool r50 = false, s54 = false;
			for (size_t o = 240; o + 2 <= n - 28 && b[o] != 255; o += 2 + b[o + 1]) {
				if (b[o] == 50) r50 = memcmp(b + o + 2, "\xac\x14\x0a\x02", 4) == 0;
				if (b[o] == 54) s54 = memcmp(b + o + 2, "\xac\x14\x0a\x01", 4) == 0;
			}
			assert(r50 && s54);
			ap_dhcp_reply(b, 5);
			/* and ask who 172.20.10.2 is: the station must answer */
			std::vector<uint8_t> q = { 0, 1, 8, 0, 6, 4, 0, 1 };
			q.insert(q.end(), kApMac, kApMac + 6); be32put(q, kApIp);
			q.insert(q.end(), 6, 0); be32put(q, kStaIp);
			ap_to_sta(kBcast_sim, 0x0806, q);
		}
		return;
	}
	if (p[9] == 1) {                                   /* ICMP echo to 172.20.10.1 */
		assert(memcmp(dst, kApMac, 6) == 0 && p[20] == 8 && ap_csum(p + 20, n - 20) == 0);
		ap_pings++;
		if (scenario == 33 && p[24] == 0xd0 && p[25] == 0xe1) {   /* step 8's last ping: the hotspot sends us away */
			ap_mgmt_to_sta(0xc0, { 4, 0 }, false);
			return;
		}
		if (scenario == 33 && ap_pings == 4) {         /* the first ping over wl0 */
			std::vector<uint8_t> q = { 0, 1, 8, 0, 6, 4, 0, 1 };   /* who has 172.20.10.2: iOS's stack answers now */
			q.insert(q.end(), kApMac, kApMac + 6); be32put(q, kApIp);
			q.insert(q.end(), 6, 0); be32put(q, kStaIp);
			ap_to_sta(kBcast_sim, 0x0806, q);
			std::vector<uint8_t> v6(40, 0);                /* an IPv6 header (router advertisement-like), no payload */
			v6[0] = 0x60; v6[6] = 58; v6[7] = 255;
			ap_to_sta(kBcast_sim, 0x86dd, v6);
			ap_to_sta(kBcast_sim, 0x0806, q, kStaMac);     /* a broadcast from us, repeated by the AP */
		}
		std::vector<uint8_t> icmp(p + 20, p + n);
		icmp[0] = 0; icmp[2] = 0; icmp[3] = 0;
		uint16_t c = ap_csum(icmp.data(), icmp.size());
		icmp[2] = (uint8_t)(c >> 8); icmp[3] = (uint8_t)c;
		ap_to_sta(kStaMac, 0x0800, ap_ipv4(1, kApIp, kStaIp, icmp));
		return;
	}
	assert(!"unexpected IP packet from the station");
}

/* a frame the station sent (HTT TX_FRM) */
static void
ap_frame(const uint8_t *f, uint32_t n, bool mgmt)
{
	assert(n >= 24 && memcmp(f + 4, kApMac, 6) == 0 && memcmp(f + 10, kStaMac, 6) == 0);
	if (mgmt) {
		if (f[0] == 0xb0) {                                   /* authentication */
			assert(n == 30 && f[24] == 0 && f[25] == 0 && f[26] == 1 && f[27] == 0 && ap_vdev_starts == 1 && ap_peer_creates == 1);
			ap_auths++;
			ap_state = 1;
			ap_mgmt_to_sta(0xb0, { 0, 0, 2, 0, 0, 0 }, false);
		} else if (f[0] == 0x00) {                            /* association request: cap, listen, SSID, rates, xrates, RSN */
			assert(ap_state >= 1);
			ap_assocs++;
			uint16_t cap = (uint16_t)(f[24] | (f[25] << 8));
			assert(cap == (0x0001 | 0x0010 | 0x0020 | 0x0400) && f[26] == 10);
			static const uint8_t want[] = { 0, 6, 'i', 'P', 'h', 'o', 'n', 'e', 1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,
				50, 4, 0x30, 0x48, 0x60, 0x6c, 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };
			assert(n == 28 + sizeof(want) && memcmp(f + 28, want, sizeof(want)) == 0);
			if (scenario == 24) {
				ap_mgmt_to_sta(0x10, { 0x31, 0x04, 17, 0, 0, 0 }, true);
				return;
			}
			ap_state = 2;
			ap_mgmt_to_sta(0x10, { 0x31, 0x04, 0, 0, 0x01, 0xc0, 1, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 }, true);
			if (scenario == 29) {
				queue_ring_frame({}, 0, false, 0, 0xFB);   /* a buffer the chip never finishes */
			}
			if (scenario != 27) {
				for (int i = 0; i < 32; i++) ap_anonce[i] = (uint8_t)(0xa0 + i);
				ap_msg1();
			}
		} else if (f[0] == 0xc0) {                            /* deauthentication (leaving) */
			assert(n == 26);
			ap_deauths++;
			ap_deauth_reason = f[24] | (f[25] << 8);
		} else {
			assert(!"unexpected management frame from the station");
		}
		return;
	}
	/* data: native wifi, to DS, LLC/SNAP; EAPOL-Key in the clear, anything else encrypted (Protected) after the keys */
	static const uint8_t snap[6] = { 0xaa, 0xaa, 0x03, 0, 0, 0 };
	assert(f[0] == 0x08 && (f[1] & 0x01) && n >= 32 && memcmp(f + 24, snap, 6) == 0);
	if (f[30] != 0x88 || f[31] != 0x8e) {
		assert(ap_authorized == 1 && (f[1] & 0x40));   /* only once the port is open, and marked for the chip's CCMP */
		ap_ip(f + 16, (uint32_t)(f[30] << 8 | f[31]), f + 32, n - 32);
		return;
	}
	static const uint8_t llc[8] = { 0xaa, 0xaa, 0x03, 0, 0, 0, 0x88, 0x8e };
	assert(f[1] == 0x01 && memcmp(f + 16, kApMac, 6) == 0 && n >= 32 + 99 && memcmp(f + 24, llc, 8) == 0);
	const uint8_t *e = f + 32;
	size_t el = n - 32;
	assert(e[1] == 3 && e[4] == 2 && (size_t)((e[2] << 8) | e[3]) + 4 == el && e[0] == 2);
	uint16_t ki = (uint16_t)((e[5] << 8) | e[6]);
	uint64_t rc = 0;
	for (int i = 0; i < 8; i++) rc = (rc << 8) | e[9 + i];
	assert(rc == ap_replay && e[7] == 0 && e[8] == 0);
	std::vector<uint8_t> z(e, e + el);
	memset(&z[81], 0, 16);
	if (ki == 0x010a) {                                       /* message 2 */
		memcpy(ap_snonce, e + 17, 32);
		ap_derive_ptk();
		uint8_t h[20];
		ap_hmac(ap_ptk, 16, z, h);
		if (memcmp(h, e + 81, 16) != 0) {
			ap_msg2_bad++;
			if (ap_msg1s < 3) {
				ap_msg1();
			} else {
				ap_mgmt_to_sta(0xc0, { 15, 0 }, false);       /* 4-way handshake timeout */
			}
			return;
		}
		ap_msg2_ok++;
		static const uint8_t ours[22] = { 48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0 };
		assert(((e[97] << 8) | e[98]) == 22 && memcmp(e + 99, ours, 22) == 0);
		/* message 3: RSN element + GTK KDE (key id 1), padded, AES key wrap with the KEK */
		std::vector<uint8_t> kd = ap_rsn();
		kd.insert(kd.end(), { 0xdd, 22, 0x00, 0x0f, 0xac, 1, 0x01, 0x00 });
		kd.insert(kd.end(), kGtk, kGtk + 16);
		kd.push_back(0xdd);
		while (kd.size() % 8) kd.push_back(0);
		std::vector<uint8_t> w(kd.size() + 8);
		AES_KEY k;
		AES_set_encrypt_key(ap_ptk + 16, 128, &k);
		assert(AES_wrap_key(&k, NULL, w.data(), kd.data(), (unsigned)kd.size()) == (int)w.size());
		ap_replay++;
		ap_state = 3;
		if (scenario == 30) {
			for (uint32_t pid : { 6u, 1u }) {
				for (uint32_t act : { 0u, 1u }) {
					std::vector<uint8_t> ev;
					put32(ev, 0x4005u);
					tlv(ev, 230, { pid, act, 1, 76, pid == 6 ? 1u : 0u });
					outq[2].push_back(htc(2, ev));
				}
			}
		}
		ap_send_ek(0x13ca, w, true);
	} else if (ki == 0x030a) {                                /* message 4 */
		assert(ap_state == 3 && e[97] == 0 && e[98] == 0);
		uint8_t h[20];
		ap_hmac(ap_ptk, 16, z, h);
		assert(memcmp(h, e + 81, 16) == 0);
		ap_msg4_ok++;
		ap_state = 4;
	} else {
		assert(!"unexpected EAPOL-Key frame");
	}
}

/* step 8: data frames in slots (msdu ids 64-79) confirmed together, several ids in one TX_COMPL_IND */
static std::vector<uint16_t> &ap_pend = *new std::vector<uint16_t>;
static uint64_t ap_pend_since;
static uint32_t ap_pend_max, ap_multi_compl;

static void
ap_flush_compl(void)
{
	std::vector<uint8_t> m = { 7, 0, (uint8_t)ap_pend.size(), 0 };
	for (uint16_t i : ap_pend) { m.push_back((uint8_t)i); m.push_back((uint8_t)(i >> 8)); }
	while (m.size() % 4) m.push_back(0);
	outq[1].push_back(htc(1, m));
	ap_multi_compl += ap_pend.size() > 1;
	ap_pend.clear();
}

static void
ap_tick(void)   /* from chip_step */
{
	if (!ap_pend.empty() && now_us - ap_pend_since >= 2000) ap_flush_compl();
}

/* HTT TX_FRM (the HTC payload p, plen bytes): check the descriptor, find the frame, complete it */
static void
ap_tx_frm(const uint8_t *p, uint32_t plen)
{
	assert(plen >= 16);
	uint8_t flags0 = p[1];
	uint16_t flags1 = (uint16_t)(p[2] | (p[3] << 8)), len = (uint16_t)(p[4] | (p[5] << 8)), id = (uint16_t)(p[6] | (p[7] << 8));
	uint32_t frags; memcpy(&frags, p + 8, 4);
	assert(p[12] == 0xff && p[13] == 0xff && p[14] == 0 && p[15] == 0);
	uint32_t type = flags0 >> 5, tid = (flags1 >> 6) & 0x1f;
	assert((flags0 & 1) && (flags1 & 0x3f) == 0 && (flags1 & (1u << 11)));
	const uint8_t *f;
	if (type == 3) {
		assert(tid == 17);
		f = resolve(frags, len);
	} else {
		assert(type == 1 && tid == 16);
		uint32_t fl[4]; memcpy(fl, resolve(frags, 16), 16);
		assert(fl[1] == len && fl[2] == 0 && fl[3] == 0);
		f = resolve(fl[0], len);
		bool eapol = len >= 32 && f[30] == 0x88 && f[31] == 0x8e;
		assert(eapol ? (flags0 & 4) != 0 : (flags0 & 4) == 0);   /* EAPOL in the clear, the rest encrypted by the chip */
	}
	uint32_t pre = ((len < 46 ? len : 46) + 3) & ~3u;
	assert(plen == 16 + pre && memcmp(p + 16, f, len < pre ? len : pre) == 0);
	ap_txfrm++;
	std::vector<uint8_t> frame(f, f + len);
	if (type == 3) {   /* management: MGMT_TX_COMPL_IND, 12-byte form as this firmware sends it (desc_id, status 0) */
		outq[1].push_back(htc(1, { 0xe, 0, 0, 0, (uint8_t)id, (uint8_t)(id >> 8), 0, 0, 0, 0, 0, 0 }));
	} else if (id >= 64 && (scenario == 33 || scenario == 34)) {   /* step 8 slots: confirmed 4 at a time (or after 2 ms) */
		assert(id < 80 && std::find(ap_pend.begin(), ap_pend.end(), id) == ap_pend.end());
		if (ap_pend.empty()) ap_pend_since = now_us;
		ap_pend.push_back(id);
		ap_pend_max = ap_pend.size() > ap_pend_max ? (uint32_t)ap_pend.size() : ap_pend_max;
		if (ap_pend.size() >= 4) ap_flush_compl();
	} else if (!(scenario == 30 && len == 32 + 99 && f[32 + 5] == 0x03)) {   /* data: TX_COMPL_IND, acknowledged */
		outq[1].push_back(htc(1, { 7, 0, 1, 0, (uint8_t)id, (uint8_t)(id >> 8), 0, 0 }));
	}   /* scenario 30: message 4 (key information 0x030a) is never confirmed */
	ap_frame(frame.data(), len, type == 3);
}

/* the WMI commands steps 5 and 6 send; true if handled (the credit goes back in the caller) */
static bool
ap_wmi(uint32_t id, const uint8_t *p, uint32_t plen)
{
	uint32_t w[40] = { 0 };
	memcpy(w, p + 4, plen - 4 < sizeof(w) ? plen - 4 : sizeof(w));
	uint32_t tag = w[0] >> 16, tl = w[0] & 0xffff;
	switch (id) {
	case 0x9001: assert(tag == 108 && tl == 8 && w[1] == 0 && w[2] == 0); ap_ps_off++; return true;
	case 0x5003: {
		assert(tag == 88 && tl == 72 && w[1] == 0 && w[3] == 100 && w[4] == 1 && w[5] == 0 && w[6] == 0);
		uint32_t *c = &w[1 + 18];
		assert((c[0] >> 16) == 80 && (c[0] & 0xffff) == 24 && c[1] == 2437 && c[2] == 2437 && c[4] == 1);
		assert(c[7] == (18u << 16) && ap_ps_off == 1);
		ap_vdev_starts++;
		std::vector<uint8_t> ev;
		put32(ev, 0x5001u);
		tlv(ev, 40, { 0, 0, 0, 0 });
		outq[2].push_back(htc(2, ev));
		return true;
	}
	case 0x6001: {
		assert(tag == 97 && tl == 16 && w[1] == 0 && memcmp(&w[2], kApMac, 6) == 0 && w[4] == 0 && ap_vdev_starts == 1);
		ap_peer_creates++;
		std::vector<uint8_t> m = { 3, 0, 5, 0 };
		m.insert(m.end(), kApMac, kApMac + 6);
		m.insert(m.end(), { 0, 0 });
		outq[1].push_back(htc(1, m));
		return true;
	}
	case 0x6005: {
		assert(tag == 101 && tl == 76 && memcmp(&w[1], kApMac, 6) == 0 && ap_state == 2);
		/* vdev, new, aid, flags, caps, listen, ht caps, max mpdu, density, rate caps, nss, vht caps, phymode, ht info x2,
		 * legacy count, ht count */
		uint32_t *c = &w[3];
		assert(c[0] == 0 && c[1] == 1 && c[2] == 1 && c[3] == (1u | 4u) && c[4] == 0x0431 && c[5] == 10);
		assert(c[6] == 0 && c[7] == 0 && c[8] == 0 && c[9] == 0 && c[10] == 1 && c[11] == 0 && c[12] == 1 && c[15] == 12 && c[16] == 0);
		uint32_t *a = &c[17];
		assert(a[0] == ((17u << 16) | 12));
		static const uint8_t rates[12] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6c };
		assert(memcmp(&a[1], rates, 12) == 0 && a[4] == (17u << 16) && a[5] == ((102u << 16) | 16));
		ap_pa_ok = true;
		ap_peer_assocs++;
		return true;
	}
	case 0x5005: assert(tag == 92 && tl == 16 && w[1] == 0 && w[2] == 1 && memcmp(&w[3], kApMac, 6) == 0 && ap_peer_assocs == 1); ap_vdev_ups++; return true;
	case 0x5009: {
		assert(tag == 96 && tl == 92 && w[1] == 0 && memcmp(&w[2], kApMac, 6) == 0 && w[6] == 4 && w[21] == 16 && w[22] == 0 && w[23] == 0);
		assert(w[24] == ((17u << 16) | 16) && ap_msg4_ok == 1);
		const uint8_t *key = (const uint8_t *)&w[25];
		bool pairwise = w[4] == 0 && w[5] == 0 && memcmp(key, ap_ptk + 32, 16) == 0 && ap_keys_ok == 0;
		bool group = w[4] == 1 && w[5] == 1 && memcmp(key, kGtk, 16) == 0 && ap_keys_ok == 1;
		if (pairwise || group) ap_keys_ok++; else ap_keys_bad++;
		outq[1].push_back(htc(1, { 0xb, (uint8_t)(0x04 | (pairwise ? 0x80 : 0)), 5, 0, 0, 0, 0, 0 }));   /* SEC_IND */
		return true;
	}
	case 0x6004:
		assert(tag == 100 && tl == 20 && memcmp(&w[2], kApMac, 6) == 0 && w[4] == 3 && w[5] == 1 && ap_keys_ok == 2);
		ap_authorized++;
		for (int i = 0; i < 3; i++) {   /* traffic from the network (decrypted by the chip): mDNS-like multicast */
			std::vector<uint8_t> f = { 0x08, 0x02, 0, 0, 0x01, 0x00, 0x5e, 0, 0, 0xfb };
			f.insert(f.end(), kApMac, kApMac + 6);
			f.insert(f.end(), { 0x02, 0x99, 0, 0, 0, 1, 0, 0, 0xaa, 0xaa, 0x03, 0, 0, 0, 0x08, 0x00 });
			f.resize(90);
			queue_ring_frame(f, 1u << 31, true, 50, scenario == 29 ? 0xFC : 0xFE);
		}
		return true;
	case 0x5007: assert(tag == 94 && w[1] == 0); ap_vdev_downs++; ap_order_ok = ap_order_ok && ap_vdev_stops == 0; return true;
	case 0x6002: assert(tag == 98 && tl == 12 && memcmp(&w[2], kApMac, 6) == 0); ap_peer_deletes++; ap_order_ok = ap_order_ok && ap_vdev_stops == 0; return true;
	case 0x5006: {
		assert(tag == 93 && w[1] == 0);
		ap_vdev_stops++;
		if (scenario == 35) {   /* the manager joins again: a fresh session */
			ap_sessions++;
			ap_authorized_total += ap_authorized;
			ap_ps_off = ap_vdev_starts = ap_peer_creates = ap_peer_assocs = ap_msg1s = ap_msg2_ok = ap_msg4_ok = 0;
			ap_keys_ok = ap_authorized = ap_state = 0;
			ap_pa_ok = false;
		}
		std::vector<uint8_t> ev;
		put32(ev, 0x5002u);
		tlv(ev, 41, { 0 });
		outq[2].push_back(htc(2, ev));
		return true;
	}
	default:
		return false;
	}
}
