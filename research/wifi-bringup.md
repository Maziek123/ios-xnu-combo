# Wi-Fi bring-up research digest (2026-11-23 to 11-25)

What was learned bringing up the QCA6174 under XNU, from the phone's own device tree, Qualcomm's and Linux's drivers,
and the phone's logs. The step-by-step results are in the main README ("Wi-Fi bring-up, step by step", progress
rows 1-20); the code is `iokit/Kernel/arm/OP2Wifi.cpp` + `OP2Ath10k.cpp`, checked with `tools/wifi-sim`.
State at the end of this period: **WPA2 connection to an iPhone hotspot works** (`wifi=8`, STEP 6 PASS); no data
has been sent through the keys yet and iOS has no network interface for it.

## Hardware truth (research/hardware/live.dts, OnePlus2_14_A.12_160226 kernel)

- Chip: **Qualcomm Atheros QCA6174 hw3.2** (PCI `168c:003e`, chip id `00342aff`, rev 10), a separate Wi-Fi + Bluetooth
  combo on PCIe, **not** the SoC's WCNSS. Bluetooth is the same chip over UART (`bt_qca6174`: reset on PMIC GPIO
  0x13, same PA/IO/crystal supplies as Wi-Fi).
- Bus: PCIe root complex 1, `qcom,pcie@fc528000` (`cell-index 1`), memory window from `0xf8800000`
  (`conf` 0xf8800000, `bars` 0xf8900000 + 7 MB). PERST = **GPIO 35**, WAKE = **GPIO 37**; supplies `vreg-1.8` (L12),
  `vreg-0.9` (L28, run at 1.0 V as the OnePlus kernel does), GDSC `pcie_1`.
- cnss node (`qcom,cnss@06300000`): WLAN_EN = **GPIO 113**, bootstrap = **GPIO 112** (both func 0, 16 mA, pull-down),
  `vdd-wlan` = `bt_vreg` (a fixed regulator on PM8994 GPIO 9), `vdd-wlan-io` = `pm8994_s4` (1.8 V, always on),
  `vdd-wlan-xtal` = L30 (1.8 V). No `vdd-wlan-sp2t` (the cnss driver can switch an SP2T antenna switch supply; this
  board does not use one). `qcom,notify-modem-status` (WLAN-LTE coexistence messages to the firmware) is not done here.
- Power-on order (cnss.c): supplies, 2 ms, bootstrap high, 10 ms, WLAN_EN high, 10 ms; then the PCIe link.
- BAR0 is 2 MB and must sit on a 2 MB boundary (`0xf8a00000`); at `0xf8900000` the chip decoded it 1 MB off.
- Crystal **48 MHz** (efuse XTAL_SEL 0): the PLL table entry is qcacld's (`ol_patch_pll_switch`); ath10k's 40 MHz
  divider value is wrong for this chip.
- Board data: the phone's own `bdwlan30.bin` (8124 bytes, board id 0). Firmware: ath10k's `firmware-6.bin`
  (`WLAN.RM.4.4.1-00309`, TLV WMI, HTT 3.87). The firmware reports **1 RF chain**.
- **Open lead: `switch_antenna`** (`oneplus,switch-antenna`, driver `drivers/input/misc/switch_antenna.c`):
  four GPIOs, **120, 121, 122, 124**, one-hot (state 0 = all low, 1-4 = one of them high), set from Android userspace
  through `/proc/antenna_switch`; the driver drives all four low at probe. What they switch is not in the source
  (a one-of-four switch fits the cellular antennas better than Wi-Fi). Under iOS nobody drives them.

## What the firmware needs (each found from a failing run)

| Need | Symptom without it | Source |
|---|---|---|
| Rewrite the chip's interrupt-enable register every 10 ms while waiting for the boot ROM | ROM never says "initialized" | ath10k `ath10k_pci_wait_for_target_init` |
| PLL switch to the 48 MHz crystal before the firmware download | WMI ready never comes (one of two causes) | qcacld `ol_patch_pll_switch`, ath10k `enable_pll_clk` |
| Copy engine 5 override in the target config (CE5 host-to-target, HTT on pipe 1) | firmware crashes after WMI init, no crash flag (only a register dump in its debug log) | ath10k `ath10k_pci_override_ce_config` |
| HTT rx ring (+ aggregation config) before anything else | scan visits every channel, 0 frames | ath10k `ath10k_htt_setup` |
| Start-up settings: chain mask for the board's RF chains, ANI on | fewer networks heard | ath10k `ath10k_start` |
| Read buffers the firmware **names** (in-order indications, rx full reorder), offloaded ones too (`htt_rx_offload_msdu` header, no rx descriptor) | EAPOL message 1 never seen | ath10k `ath10k_htt_rx_in_ord_ind` |
| Find finished buffers by scanning all of them, not in ring order | the chip takes a buffer, never finishes it, and blocks every later frame (the scans' "weak reception" was mostly this) | (our own finding) |
| Management frames go out as HTT TX_FRM (no WMI management tx on this firmware) and complete as **MGMT_TX_COMPL_IND** (type 14), not TX_COMPL_IND | "no transmit completion" for the first authentication frame | ath10k `ath10k_mac_tx_h_get_txpath`, HTT 3.x |
| Don't wait for message 4's completion; install the keys right after sending it | the firmware never confirms message 4 (an MCC tx pause stop/wake comes with message 3) | wpa_supplicant |
| Decode WMI TX_PAUSE (0x4005); hold sends while a vdev pause is active | (only seen so far around message 3) | ath10k `ath10k_wmi_tlv_event_tx_pause` |

Smaller facts: hw3.2 needs the rx filter reset (vdev create/delete + barrier) before the first scan; the TLV
firmware inverts `WMI_SCAN_FILTER_PROBE_REQ`; WMI init with 0 host memory chunks; key install is confirmed by an
HTT SEC_IND and also a WMI install-key-complete event (0x5003); PEER_STATE events (0x6005) follow the authorize.

## Radio observations (open)

- Reported noise floor **-106 to -120 dBm** on every run (2.4 GHz about -119), below what a working receiver
  reports (about -95 to -105). Unchanged by the chain mask. Either the firmware reports it differently on this board
  or the receive gain/calibration is off.
- `rx_frame_count` in the channel reports is always 0 (busy/rx_clear moves), so "receiving 0 %" means nothing.
- With the buffer fix a 5.6 s scan collects about 20 beacons from 5 networks, the hotspot next to the phone at
  -41 to -54 dBm. 5 GHz from the house router (-72 to -78 dBm) reads stronger than its 2.4 GHz (-84 dBm).
- Transmit works: authentication, association and EAPOL frames are acknowledged by the hotspot.

## Next

1. Data through the keys: DHCP with the hotspot (proves encryption both ways), then an `ifnet` for iOS like
   `ue0` (bsd/dev/op2_usb.c) and configd service publication like `op2shell` does for USB.
2. Read GPIO 120-124 under iOS (and drive them low as Android does at probe) to rule the antenna switch in or out.
3. Noise floor: compare with the firmware's own reports under Android if a log can be had there.

## References

- Linux ath10k (`drivers/net/wireless/ath/ath10k`: pci.c, ce.c, htc.c, htt_tx.c, htt_rx.c, wmi-tlv.c, mac.c, hw.c).
- qcacld-2.0 (the phone's Android driver; LineageOS OnePlus 2 kernel): `ol_fw.c` (PLL, OTP), `dbglog.h`.
- OnePlus kernel OnePlus2_14_A.12_160226: `drivers/net/wireless/cnss/cnss.c`, `drivers/pci/host/pci-msm.c`,
  `drivers/input/misc/switch_antenna.c`.
- IEEE 802.11-2016 12.7 (4-way handshake), RFC 2202 (HMAC-SHA1 vectors), RFC 3394 (AES key wrap), the 802.11i PRF
  test vector.
