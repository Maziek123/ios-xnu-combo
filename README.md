# iOS on OnePlus 2

Apple **iOS 12.5.7** (`16H81`, built for the iPhone 6 Plus, `n56ap`) running on a **OnePlus 2** (`ONE A2003`,
Qualcomm Snapdragon 810 / MSM8994) through a custom port of Apple's open-source XNU kernel. The phone boots from
Qualcomm's own bootloader into a small shim, XNU takes over, mounts a real HFS+ root on the eMMC, and runs
launchd, backboardd and SpringBoard to the Lock Screen and Home Screen.

The kernel lives in its own repository: [Veerhooda/xnu](https://github.com/Veerhooda/xnu) (branches `main` and `msm8994`, kept identical);
clone it to `xnu/xnu-4903.270.47` inside this checkout.

---

## Status (2026-11-24)

| Area | State | Evidence |
|---|---|---|
| Boot | LK `boot.img` → shim → XNU → launchd → SpringBoard, Lock Screen and Home Screen | every round in `logs/` |
| CPU | 4 × Cortex-A53 with thermal management (TSENS, throttle at 70 °C, cutoff at 90 °C) | `OP2 TOP:` lines with temperature |
| Storage | SDHCI eMMC 5.0, multi-block reads (CMD18) with a boot self-test, journaled HFS+ root | `OP2 eMMC MULTI: … fails=0` |
| Display | MDP5 command-mode panel, double-buffered (`op2fb_double`, default on), SwapWait honoured; block downsampler 1242×2208 → 1080×1920 in ~6 ms, ~8 ms per present | `OP2 FB PERF:` / `OP2 FB: downsample` lines |
| Input | Synaptics S3320 touch; Home = a touch starting on the bottom strip (centre third), sent through `OP2Buttons`; no synthetic input by default (`op2_autoinput=0`) | `OP2I2C: Home key` lines |
| **GPU (Adreno 430)** | **Composites the UI on the hardware** (`metal=3`: home/lock screens, dock blur); OrangeTree game still blank (SpriteKit pipelines fail in the shim) | [docs/GPU-PLAN.md](docs/GPU-PLAN.md) |
| **USB serial + shell** | CDC-ACM live kernel log; root `dash` shell with job control and toybox applets | [docs/USB-CONSOLE-PLAN.md](docs/USB-CONSOLE-PLAN.md) |
| **USB Ethernet + SSH** | Composite CDC-ECM (`ue0` 192.168.7.2), `ping` works; dropbear SSH: exec, `scp -O`, pubkey auth | [docs/USB-CONSOLE-PLAN.md](docs/USB-CONSOLE-PLAN.md) |
| Battery | `OP2Battery` (PMI8994 fuel gauge → `IOPMPowerSource`) is in the kernel; not yet checked against a meter | kernel source |
| **Internet over USB** | Through the Mac's NAT: configd makes `ue0` primary, DNS + HTTP verified from the phone ([setup](docs/SETUP.md)) | `op2net example.com` → `200 OK` |
| Safari | Loads sites over HTTPS with JavaScript (JIT on): Google, Wikipedia, YouTube, apple.com (heavy sites still rough) | photos on the phone |
| **Wi-Fi (QCA6174 on PCIe)** | Bring-up, no networking yet: the chip is powered, the PCIe link is up and **the chip boots its own firmware** (`WLAN.RM.4.4.1`) and it answers commands (WMI ready, MAC address read); a first scan is next ([below](#wi-fi-bring-up-step-by-step)) | `OP2 WIFI: RESULT:` lines (boot-arg `wifi=5`) |
| Not working yet | Wi-Fi networking and Bluetooth, RTC (clock does not keep time) | [docs/NEXT-PLAN.md](docs/NEXT-PLAN.md) |
| Known issues | brief pale/partial frames while scrolling; logd SIGBUS/SIGSEGV loop, Spotlight extensions SIGABRT loop, occasional SpringBoard SIGSEGV | [CLAUDE.md](CLAUDE.md) |

Between 2026-11-14 and 11-16 another AI agent worked on this tree. Its SpringBoard injection (a 2048 game inside
SpringBoard, a passcode-lockout bypass, scripted input) left the phone slow and locked out; it was removed on
2026-11-17 and the claims it added to these docs (2 ms bilinear downsampler, "all apps fully functional",
`op2fb_client_ui=1` required) were wrong and have been corrected. The standalone 2048 app (`userland/games/2048`)
is kept but has not been re-verified.

### GPU bring-up, stage by stage

Everything here was measured on the phone; the details and logs are in [docs/GPU-PLAN.md](docs/GPU-PLAN.md).

| Stage | Result |
|---|---|
| **G0** kernel driver | Command processor really executes (it never had: it was "halted" with the wrong bits), GPU SMMU set up through TrustZone (SMC `restore_sec_cfg` after a secure page-table pool), core clock voted through the RPM, memory-write fences, hang detection and recovery. 1000/1000 checked submissions, 1000-submission burst through a ring wrap. |
| **G1** command-processor blits | Userland library [`userland/op2gpu`](userland/op2gpu) (fill and copy byte-exact on the phone, same hash as the host's fake GPU). |
| **G2** shader compiler | Mesa 25.0.7 + hakios's Metal (AIR) front end built on the Mac, patched for iOS 12's AIR. **Every CoreAnimation pipeline the phone actually asked for compiles for the A430**; 61 of 62 QuartzCore functions with all constants zero. |
| **G3** first draw | Mesa's own fd4 driver, cross-built for iOS 12, draws on the phone through our kernel: pixel exact. |
| **G4** what CoreAnimation needs | Texture sampling, varyings, constant buffers, premultiplied blending, BGRA8 targets at full panel size (2 073 600 of 2 073 600 pixels exact), CoreAnimation's real uber shader with its captured function constants. |
| **G4c** stray writes (fixed) | The uber shader at full panel size wrote outside its buffers. Root cause was dead private-memory stores past fd4's fixed 8 KB buffer; `tools/mesa-op2/ir3-fd4-pvtmem.patch` drops them. |
| **G4d/G4e** SMMU translation | Real SMMU translation for the A430 (boot-arg `gpu_xl`, off by default, 4 KB identity pages): stray GPU writes fault and are dropped instead of corrupting kernel memory. Full-size draws pixel-correct with zero faults. |
| **G5** Metal on the Adreno | `op2mtl` (Mesa fd4 behind a C API) plus the `OP2Metal` shim: with `metal=3` CoreAnimation composites the home screen, lock screen and dock blur on the GPU. Open: the OrangeTree game shows blank (SpriteKit pipelines return NULL). |

### Wi-Fi bring-up, step by step

The Wi-Fi/Bluetooth chip is a Qualcomm Atheros **QCA6174** (hw3.2) on PCIe root complex 1, not the SoC's WCNSS.
The kernel code is `iokit/Kernel/arm/OP2Wifi.cpp` (power, PCIe, chip registers) and `OP2Ath10k.cpp` (copy
engines, boot loader and firmware download, ported from Linux's ath10k). It is off by default; the boot-arg `wifi` in the `CMDLINE=`
line of `tools/build-daily.sh` sets how far it goes. It runs in its own kernel thread 120 s after boot and repeats
its result every 60 s so the line survives in ramoops: `grep -a 'OP2 WIFI' op2-last-boot.log`.

| Step | `wifi=` | Result |
|---|---|---|
| probe | 1 | Read-only: logs the chip's pins, PMIC supplies, PCIe clocks and power domain. |
| **1** power + PCIe link | 2 | **Passes on the phone.** Chip power (bt_vreg, L30, bootstrap, WLAN_EN), the controller's supplies, clocks and TrustZone secure config, the 20 nm PHY and link training: the chip answers as `168c:003e`, class/rev `02800032`, link x1 Gen1. |
| **2a** chip registers | 2 | **Passes on the phone.** BAR0 (2 MB, on a 2 MB boundary at `0xf8a00000`), SoC wake, chip ID `00342aff` (rev 10 = hw3.2), readable about 80 ms after WLAN_EN; no reset needed. |
| **2b** DMA + boot loader | 3 | **Passes on the phone.** The copy engines (CE0/CE1 for boot-loader messages, CE7 as a window into the chip's memory), the target-side configuration and the first boot-loader (BMI) command, `GET_TARGET_INFO`: the chip answers version `05030000` (hw3.2) with no reset. |
| **2c** firmware boot | 4 | **Passes on the phone.** Board data (the phone's own `bdwlan30.bin`), the OTP calibration program and the firmware (`WLAN.RM.4.4.1-00309`, 681 KB in six segments through the diagnostic window), then `BMI_DONE`: 11 ms later the firmware sends its HTC ready message (2 send credits of 1792 bytes, 4 endpoints, HTC 2.1). Uses ath10k's `firmware-6.bin`, which stays out of this repository with the board data ([below](#wi-fi-firmware-files)). |
| **3** firmware services | 5 | The remaining copy engines (CE2-CE5), HTC connect of the firmware's data (HTT) and control (WMI) services, the WMI service-ready event (any host memory the firmware asks for), WMI init with ath10k's resource config, then the WMI ready event, which carries the chip's Wi-Fi MAC address; also the HTT version and a WMI echo. **Passes on the phone** (third run, progress rows 10-12): the copy-engine 5 override ath10k applies to QCA6174 was the missing piece. |
| **4** listen-only scan | 6 | **Passes on the phone, weakly**: with ath10k's HTT rx ring the chip hears networks (2 in 5.6 s, beacons through the rx ring), but far fewer than are around; third version (ath10k's start-up settings: chain mask, ANI) not yet run. Steps: the board's regulatory domain, a channel list with every channel passive (2.4 GHz 1-13, 5 GHz 36-165), a station interface with the chip's MAC, then a scan; the log lists the networks heard (name, channel, signal, security). The chip does not transmit. |
| **5** join | 7 | Written, not yet run on the phone: finds the network named in `wifi-network.txt` ([below](#wi-fi-firmware-files)) in the scan (scans up to twice more if it is not there), checks it offers WPA2-PSK with AES (CCMP), starts the chip on its channel, then open authentication and association as a plain 802.11a/b/g station. Frames go out through HTT (the firmware has no WMI management transmit). Leaves the network politely at the end. |
| **6** WPA2 | 8 | Written, not yet run on the phone: the 4-way handshake (keys from the network name and password, message 3's MIC checked, the group key decrypted), both keys into the chip, the port opened, then 5 s connected counting data frames from the network. The crypto (SHA-1, HMAC, the 802.11 PRF, AES key unwrap) is in the driver and checks itself against published test vectors first. |

After joining: a network interface for iOS (DHCP through configd), staying connected (power save, beacon loss, roaming), then Bluetooth.

#### Progress so far (2026-11-23 to 11-24)

| # | What ran (commit) | What the log showed | What it told us |
|---|---|---|---|
| 1 | Read-only probe, `wifi=1` (`28c5cb3`) | Everything for Wi-Fi off: the PCIe controller's power domain collapsed, its clocks gated, its resets held | Under iOS nothing powers the chip, so the driver has to do the whole power-up. (The PMIC's L28/L30 registers read as zero from our side; the RPM still accepts requests for them.) |
| 2 | Step 1, `wifi=2` (`28c5cb3`) | **PASS** on the first run: `168c:003e`, class/rev `02800032`, link x1 Gen1; every RPM request acked, the TrustZone call returned 0 | The power sequence, clocks, PHY table and link training taken from Qualcomm's `cnss.c` and `pci-msm.c` work under XNU. |
| 3 | Step 2a (`5ef26ed`) | FAIL: the chip's SoC never woke (`rtc_state 00000000`) | BAR0 sat at `0xf8900000`, not aligned to its 2 MB size; the chip decoded it at `0xf8800000`, so every access landed 1 MB off. |
| 4 | BAR0 on a 2 MB boundary, checked by reading it back (`423f271`) | The SoC woke in 50 µs, but the chip ID and firmware indicator read `00000000` | Read too early: ath10k once had the same symptom (the ID reads 0 until the chip has initialised). |
| 5 | Poll for up to 5 s, cold reset only as a fallback (`9290d11`) | **PASS**: chip ID `00342aff` (hw3.2), valid 83 ms after WLAN_EN, no reset needed; the firmware indicator stayed 0 | Register access works, and ath10k's cold reset, its riskiest step, isn't needed. |
| 6 | Step 2b, `wifi=3` (`20094ef`) | **First DMA works**: the chip read its own memory through copy engine 7 into phone RAM (`pcie_state at 00401ee0`), no crash. PARTIAL: the boot ROM never reported "initialized" in 10 s, so BMI wasn't tried | After a plain power-on the ROM doesn't raise that flag. The chip's copy engines see their own memory at `0xf8a00000`, the BAR0 address; our DMA block (`39e8d000`) is well clear of it. |
| 7 | Try BMI without waiting; if it stays silent, reset only the chip's CPU and try again (`123f043`) | The chip's memory reads and writes all work, and the configuration read back correctly. But BMI got no answer, before or after the CPU reset: the chip never set up its side of the boot-loader channels (copy engine 0 has no receive ring, copy engine 1 no send ring) and never reported "initialized" | The boot ROM starts (it changes its reset-control register and has its tables in memory) but stops before announcing itself. Both reference drivers keep writing the chip's interrupt-enable register while they wait; we never did. |
| 8 | Wait the way ath10k does (interrupt enable rewritten every 10 ms, with the chip's interrupt-disable bit set so nothing reaches the phone); register dumps; cold reset only if BMI stays silent (`c13dd84`) | **PASS** (no reset): the ROM reported "initialized" 440 ms into the wait (1.1 s after WLAN_EN), and BMI answered version `05030000` (hw3.2), type 8 | Most likely the ROM waits for the host to enable its interrupt: without those writes it stayed silent for 10 s. No reset of any kind is needed. |
| 9 | Step 2c, `wifi=4`: firmware download and boot; all DMA memory now non-cacheable, so several receive buffers can be outstanding (`f05fb2a`) | **PASS** on the first run: board data to `00401fc0`, OTP executed (result 2), six firmware segments through the diagnostic window; the firmware's HTC ready came 11 ms after `BMI_DONE`: 2 credits of 1792 bytes, 4 endpoints, HTC 2.1 | The chip runs its real firmware. OTP result 2 is not an error: this firmware's feature flags tell ath10k to ignore the OTP result, and we do the same. 2 credits of 1792 bytes is also what ath10k logs for its other PCIe chips. |
| 10 | Step 3, `wifi=5`: HTC services, WMI init and ready, HTT version (`9bcbfb7`) | FAIL: both services connected (HTT endpoint 1, WMI endpoint 2), the WMI service-ready event came (TLV ABI matches; 1 RF chain, regdomain `0x6c`, no host memory wanted) and WMI init was delivered, but no WMI ready in 5 s; no crash | Everything up to WMI init works. Linux's ath10k and the phone's own Android driver (qcacld-2.0) both switch the chip onto its PLL clocks before downloading the firmware, set up for the board's crystal; we never did, so the firmware's radio side may have been running off the bare crystal. qcacld also runs the OTP once for the board ID first. |
| 11 | Step 3 again: the PLL switch both host drivers make, the OTP board-ID run, the firmware's debug log on, a longer wait with diagnostics (`ad6d248`) | FAIL, but the firmware's debug log held a crash register dump with a backtrace; one register pointed at entry 5 of the copy-engine table we wrote in 2b. PLL switch worked (48 MHz crystal) | ath10k overrides that entry for QCA6174 (`ath10k_pci_override_ce_config`): copy engine 5 is the firmware's own, and HTT messages go to pipe 1. Our port of 2b missed it. The crash also never raised the firmware's crash flag. |
| 12 | Step 3 with the CE5 override (target table entry 5, service map, no host ring on CE5; HTT on CE1) (`6cbea42`) | **PASS**: WMI ready 13 ms after init (status 0, the chip's MAC address, a Qualcomm one), HTT version 3.87, WMI echo answered, credits come back | The firmware is fully up and takes commands. The PLL switch stays: both host drivers make it. |
| 13 | Step 4, `wifi=6`: listen-only scan (`5064ce7`) | **FAIL**: the scan started, visited all 37 channels and completed in 5.6 s, but no beacon arrived (0 frames) | The radio tunes but the firmware delivers nothing: ath10k hands the firmware an HTT rx ring (with its aggregation config) before anything else, and the firmware's receive path needs it. |
| 14 | Step 4 with the HTT rx ring (127 buffers of 2 KB, ath10k's order: rx filter reset, ring, then regdomain and scan), per-channel radio reports (noise floor, busy and receive time) and beacons taken from the ring as well as from WMI (`bc129f5`) | **PASS, weak**: 2 networks (strongest -40 dBm), 3 beacons and 2 control frames in 5.6 s, all through the rx ring (none over WMI, no HTT rx indications); noise floor -107 to -118 dBm, air busy only around channel 11 | The receive path works now, but the radio hears only a strong nearby access point. The reported noise floor is below what a working receiver measures (about -95 to -105 dBm). ath10k also sets the chain mask to the board's one RF chain (we left the init's 7) and turns ANI on. |
| 15 | Step 4 with ath10k's start-up settings (PMF QoS, dynamic bandwidth, idle power save, tx/rx chain mask = 1, ARP access category, ANI on) | Not run yet | Checked in simulation (order and values). |
| 16 | Steps 5 and 6, `wifi=8`: join an iPhone hotspot (WPA2) and run the 4-way handshake (`f80b533`) | **FAIL** at the first authentication frame: the hotspot was heard on the second scan (-42 dBm), the chip started on its channel and mapped the peer, the frame went out, but its completion came as HTT type 14 (MGMT_TX_COMPL_IND), which the driver did not read | For management frames sent through HTT this firmware reports completion with its own message (frame id, status 0 ok / 1 no ack / 2 dropped). Read now. |
| 17 | Step 5 reading MGMT_TX_COMPL_IND (`6191090`) | Run 1: **FAIL**, the hotspot not heard in 3 listen-only scans (4-5 networks each, 1-2 frames per network). Run 2, same image: **STEP 5 PASS**, the first association: hotspot heard on the second scan (-48 dBm), authentication and association requests acknowledged, AID 1. Step 6 **FAIL**: no message 1 within 10 s, although the firmware announced one 131-byte frame (the size of message 1) with an in-order rx indication | Transmit works both ways. Reception still misses most beacons. In rx full reorder mode the firmware names the buffers it filled (some offloaded: a short header instead of the rx descriptor); the driver only looked at buffers in ring order and never at that one. |
| 18 | Step 5 asks for the network by name: probe requests on 2.4 GHz channels 1-11 (12-13 stay listen-only), up to 4 quick rounds, then one more listen-only scan of every channel | Not run yet | An access point answers a probe request with a probe response addressed to us and retries it until we acknowledge it: several chances instead of one. Checked in simulation (a hotspot that only answers probes, in the second round). |
| 19 | Steps 5 and 6 with in-order rx indications handled, probing by name (`9b1c327`) | Step 5 **PASS** (hotspot in the first scan, 4 beacons; 20 beacons in all now that a stuck buffer no longer blocks the rest). Step 6: message 1 read, message 2 acknowledged, **message 3's MIC checked** (the password and the key derivation are right), message 4 sent - the iPhone showed a connected device - but the firmware never confirmed message 4 and the driver gave up before installing the keys. Two WMI TX_PAUSE events came with message 3 | wpa_supplicant installs the keys right after sending message 4; so does the driver now. |
| 20 | Step 6 without waiting for message 4's confirmation (2 s at most), a repeated message 3 answered while connected, TX_PAUSE decoded (vdev pauses hold our sends as ath10k stops its queues; the others are only logged) | Not run yet | Checked in simulation: pause events with message 3 and message 4 never confirmed. |

#### Testing a step

1. Set `wifi=N` in the `CMDLINE=` line of `tools/build-daily.sh`, build, and boot as usual.
2. Wait about 3 minutes after the UI comes up: the Wi-Fi thread starts 120 s after boot.
3. Shut down from Settings → General → Shut Down, then in TWRP:

```bash
adb pull /sys/fs/pstore/console-ramoops-0 ./op2-last-boot.log
grep -a 'OP2 WIFI' op2-last-boot.log | grep -v 'RESULT (repeat)'
```

The `RESULT:` line is the verdict; the lines before it show where a failing step stopped.

#### Wi-Fi firmware files

Step 2c and later (`wifi=4` and up) need two files in `firmware/qca6174/` of the checkout you build from. That folder is in
`.gitignore`: the firmware is Qualcomm's and is not redistributed here.

* `firmware-6.bin`: ath10k's firmware for this chip, from linux-firmware (`ath10k/QCA6174/hw3.0/firmware-6.bin`,
  version `WLAN.RM.4.4.1`). Most Linux PCs already have it in `/lib/firmware/ath10k/QCA6174/hw3.0/`; if it is
  compressed there, unpack it: `zstd -d firmware-6.bin.zst -o firmware-6.bin` or `xz -dk firmware-6.bin.xz`.
* `bdwlan30.bin`: the phone's own board data (its calibration), 8124 bytes, from the phone's Android firmware.

`tools/build-xnu.sh` runs `tools/gen-wifi-fw.py`, which embeds both in the kernel and prints
`[wifi-fw] embedding firmware WLAN.RM.4.4.1-...`. Without them the kernel still builds, and step 2c reports that no
firmware is built in.

Steps 5 and 6 (`wifi=7`, `wifi=8`) join a network you name in a third file in the same folder,
`firmware/qca6174/wifi-network.txt`: the network's name on the first line, exactly as it appears (upper and lower
case, spaces and apostrophes count), and its password on the second (leave the second line out for an open network):

```
iPhone
your-password
```

The build prints `[wifi-net] network iPhone (WPA2-PSK) ...`. The password is not built in: `gen-wifi-fw.py` turns it into
the WPA2 key (as `wpa_passphrase` does) and the kernel only gets that key, in a header that is also gitignored. Anyone
with the boot image can still join that network, so don't share images built with it. For an iPhone hotspot, keep
**Maximize Compatibility** on (2.4 GHz, WPA2) and the Personal Hotspot screen open while the phone boots.

---

## How a test round works

Our unit's volume/power buttons are broken, so nothing here relies on them (a unit with working buttons
behaves the same). Every boot is a RAM boot of a freshly built image, with a software deadline that resets
the phone back into TWRP, where the kernel log is pulled from ramoops. The phone has a root shell two ways:
over USB serial (`screen /dev/cu.usbmodem*`, `op2#` prompt) and over SSH (`ssh root@192.168.7.2`, set the Mac
side to 192.168.7.1 first — see [docs/USB-CONSOLE-PLAN.md](docs/USB-CONSOLE-PLAN.md)).

```bash
bash tools/round.sh                 # build kernel + boot image, RAM-boot, wait for the reset, save the log
DEADLINE=300 bash tools/round.sh    # shorter run
cat logs/last-summary.txt           # summary; full logs in logs/daily-*.log
```

* `tools/build-daily.sh` builds the kernel and `out/xnu-boot.img`; the boot-args live there, in one place (`EXTRA=` appends; reference table in [docs/SETUP.md](docs/SETUP.md#51-boot-arguments)).
* `tools/boot-daily.sh` re-arms the recovery cookie and `fastboot boot`s the image. It never flashes.
* `tools/pull-log.sh` pulls `/sys/fs/pstore/console-ramoops-0` and writes the summary.
* `tools/rootfs-journal.sh` rebuilds the journaled rootfs from the clean image, installs the GPU driver, OP2Metal,
  games and the USB shell/SSH launcher with its launchd job, and streams it to userdata
  (needs `sudo` on the Mac for file ownership inside the image).

### Safety rules (the phone is hard to recover)

* Never `fastboot flash`. Boot only with `fastboot boot out/xnu-boot.img`.
* Keep the recovery (BCB) cookie armed so every reset lands in TWRP; `tools/boot-daily.sh` does it.
* Every boot image carries `op2_deadline` — the hardware watchdog strands the phone, so the software deadline is
  the only way out of a hang.
* Do not raise CPU voltages beyond the stock values used by `op2_cpu_boost`.

---

## Building the stacks

```bash
bash tools/mesa-host.sh             # Mesa + AIR front end for this Mac (out/mesa-host): air2nir, ir3_compiler
bash tools/ca-sweep.sh              # every QuartzCore function through AIR -> NIR -> ir3 for the A430
bash tools/ca-compile-captured.sh   # CoreAnimation's real pipelines, with the constants captured on the phone
bash tools/mesa-ios.sh              # Mesa + freedreno cross-built for iOS 12 (out/mesa-ios): air_run
make -C userland/op2gpu test        # the CP blit library against its fake GPU
```

Getting a program onto the phone and started:

* `tools/op2hfs.py` reads and rewrites files on the HFS+ userdata partition in place over adb (TWRP has no hfsplus).
  It cannot grow a file.
* iOS 12's launchd reads its jobs only from `/System/Library/Caches/com.apple.xpcd/xpcd_cache.dylib`.
  `tools/xpcd-cache-add.py` adds a job to that cache without changing the file's size (boot-arg
  `launchd_unsecure_cache=1` lets launchd accept it). A plist dropped into `LaunchDaemons/` alone is ignored.
* `tools/fc-from-log.py` turns OP2Metal's function-constant capture lines (boot with `metal=2`) into compiler input.

Host requirements: macOS on Apple Silicon, Xcode 10.3 at `~/xcode-10.3` (its iPhoneOS 12.4 SDK
and libc++ headers; override with `XCODE_ROOT`), Python 3, `adb`/`fastboot`.
Mesa's build tools (meson, ninja, bison 3.8) are installed by the scripts into `out/`, not system-wide.
`sh`/`dash`/`toybox`/`dropbear` userland pieces: `bash tools/build-toybox.sh`, `bash tools/build-dropbear.sh`,
then `bash tools/build-op2shell.sh` (embeds everything + your `~/.ssh/id_ed25519.pub`; `--install` in TWRP).
See [docs/SETUP.md](docs/SETUP.md) for the full replication guide (file placement, build order, loop).

---

## Repository layout

* [`shim/`](shim/) — bare-metal boot shim between Qualcomm LK and XNU.
* [`tools/`](tools/) — build, boot, log, rootfs, Mesa and GPU test tooling (see above).
  * [`tools/mesa-op2/`](tools/mesa-op2/) — our patches on top of Mesa and the AIR front end (iOS 12 AIR, `air_run` options).
  * [`tools/ios-iokit/`](tools/ios-iokit/) — the few IOKit user-client declarations the iOS SDK lacks headers for.
  * [`tools/games/`](tools/games/) — build scripts for native userland games (`build-2048.sh`).
* [`userland/OP2Metal/`](userland/OP2Metal/) — the Metal plug-in shell iOS loads for our GPU; also captures function constants.
* [`userland/op2gpu/`](userland/op2gpu/) — command-processor blit library, fake GPU, phone probe.
* [`userland/games/`](userland/games/) — native UIKit games for iOS 12 (`2048`).
* [`docs/`](docs/) — [setup/replication guide](docs/SETUP.md), [doc index](docs/README.md),
  [USB serial/shell/Ethernet/SSH](docs/USB-CONSOLE-PLAN.md), [GPU plan and results](docs/GPU-PLAN.md),
  [audit](docs/AUDIT-2026-11-11.md), [how it boots](docs/HOW-IT-BOOTS.md),
  [kernel changes](docs/OP2_KERNEL_CHANGES.md), [hacks](docs/HACKS.md), history.
* `history/` — private by design (AI chat transcripts, session prompts); gitignored, never pushed.
* [`userland/op2shell/`](userland/op2shell/) — the root-shell/SSH launcher (dash+toybox+dropbear+scp).
* `tools/env.sh` — shared host paths (`XCODE_ROOT`, SDKs); every build script sources it.
* [`CLAUDE.md`](CLAUDE.md) — short working notes: current state, loop, rules.
* `kernel-backup/` — copies of kernel sources before risky edits.
* Not in the repository: `xnu/` (its own repo), `reference/`, `research/`, `logs/`, `backup/`, `out/`, `history/` —
  device dumps, Apple binaries and build output.

---

## Credits and licences

* **Apple** — XNU (`xnu-4903.270.47`), under the Apple Public Source License 2.0.
* **[oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel)** and **hakios-metal** (MIT) —
  the AIR (Metal) front end for Mesa, the libdrm stand-in, `air_run`, and the GPU layer `userland/op2gpu` is
  adapted from. Their licence is kept as `userland/op2gpu/LICENSE-hakios`.
* **[Mesa](https://mesa3d.org)** (MIT) — freedreno, ir3, NIR. Fetched and patched at build time, not vendored.
* **Qualcomm's KGSL** (GPL, in the OnePlus kernel source) — read as the reference for the A430 bring-up sequence.
* **Linux ath10k** (ISC) — the copy-engine, diagnostic-window and BMI code in `OP2Ath10k.cpp` is ported from
  `drivers/net/wireless/ath/ath10k`; the Atheros/Qualcomm copyright lines and the ISC permission notice are kept in
  that file. The PCIe and Wi-Fi power sequences were read from Qualcomm's `pci-msm.c` and `cnss.c` (GPL, OnePlus
  kernel source).
* **[dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet)** — the method this port started from.
