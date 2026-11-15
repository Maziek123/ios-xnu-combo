# Setup & replication guide: iOS 12 on OnePlus 2

Audience: an engineer with a OnePlus 2 (ONE A2003, MSM8994) and an Apple Silicon Mac,
replicating this project from a fresh checkout. Read this, then `CLAUDE.md` (safety rules),
then `docs/NEXT-PLAN.md` (what to work on).

## 1. Host requirements

- macOS on Apple Silicon. Everything below assumes this.
- Phone note: our unit's Volume/Power buttons are broken, so every procedure here avoids them
  (recovery via the BCB cookie + software deadline). A unit with working buttons behaves the same —
  nothing here depends on them being broken.
- Xcode 10.3 (period toolchain: clang, iPhoneOS 12.4 SDK, Swift 5.0.1 for the game).
  Extract it to **`$HOME/xcode-10.3`** so that `$HOME/xcode-10.3/Xcode.app/Contents/Developer`
  exists. Override per-command if yours lives elsewhere: `XCODE_ROOT=/path/to/Xcode.app/Contents/Developer`.
- Android platform tools (`adb`, `fastboot`) on `PATH`.
- `python3`, `make`, system `clang`, `ssh`/`scp`/`ssh-keygen`.
- Homebrew `gnu-sed` (toybox build only): `brew install gnu-sed`.
- A USB-A to USB-C/micro cable that does data (the phone enumerates our gadget, adb, and fastboot over it).

## 2. Repo layout (where things live)

Clone the repo **anywhere**; all scripts resolve the root themselves. `tools/env.sh` centralises
host paths (`XCODE_ROOT`, `IPHONEOS_SDK`, `REPO_ROOT`) — every build script sources it and fails
with a placement hint when Xcode is missing.

| Path | Tracked? | Contents |
|---|---|---|
| `tools/` | yes | all build/boot/deploy scripts; `tools/env.sh` host paths |
| `userland/` | yes | `op2shell` (launcher), `op2gpu`, `OP2Metal`, `op2mtl`, `op2bench` sources |
| `docs/` | yes | plans, status, this guide |
| `kernel-backup/` | yes | before/after copies of every kernel file changed per round (`fable-NN/`) |
| `xnu/` | **no** (gitignored) | XNU 4903.270.47 source + `BUILD/` objects; back up before editing |
| `out/` | **no** | build products, extracted sources, Apple images, round logs stay in `logs/` |
| `reference/` | **no** | peer trees (hakios, ios-sweet, kernel-oneplus) used as references only |
| `logs/` | **no** | per-round kernel logs, usb-console captures, test transcripts |

## 3. One-time file placement (everything not in git)

| File | Put it at | Get it from |
|---|---|---|
| Xcode 10.3 | `$HOME/xcode-10.3/Xcode.app` | Apple developer portal (legacy downloads) |
| dash source | `out/shell/dash-0.5.12.tar.gz` (extract beside it) | dash mirror (0.5.12 used here) |
| dropbear source | `out/shell/dropbear-2026.94.tar.bz2` | https://matt.ucc.asn.au/dropbear/ |
| toybox source | `out/shell/toybox-0.8.12.tar.gz` | https://landley.net/toybox/downloads/ |
| iOS images | `out/038-87140-050.dmg`, `out/ios12-userdata-fixA.dmg`, … | user's own dumps of the n56ap iOS 12.5.7 userland (used by `tools/rootfs*.py`, `tools/mkop2root.py`); not redistributable, not in git |
| SSH key | `~/.ssh/id_ed25519` (+`.pub`) | `ssh-keygen -t ed25519` (no passphrase); the `.pub` is embedded as the phone's `authorized_keys` at launcher build time |

Toybox also needs `out/shell/toybox-ios-shim/sys/disk.h` (in git) on the include path —
handled inside `tools/build-toybox.sh`.

## 4. Build order

1. Kernel + boot image: `bash tools/build-daily.sh` → `out/xnu-boot.img` (refuses if the
   cmdline would exceed 250 bytes).
2. Shell tools (each stages into `out/shell/build/`, stripped + ad-hoc signed):
   `bash tools/build-toybox.sh`, `bash tools/build-dropbear.sh`,
   then `bash tools/build-op2shell.sh` (needs dash + toybox + dropbear + scp +
   `out/shell/build/authorized_keys`, i.e. your `.pub`).
3. GPU userland only if you touch it: `tools/mesa-host.sh`, `tools/mesa-ios.sh`,
   `userland/OP2Metal/build.sh`, `userland/op2gpu` (`make ios`).
4. Install the launcher while the phone is in TWRP: `bash tools/build-op2shell.sh --install`
   (writes the 9 MB `op2-air-run` slot in place, reads back + `cmp`s).

## 5. The round loop (ephemeral boot, never flash)

```bash
DEADLINE=7200 bash tools/round.sh
```

Builds, re-arms the BCB recovery cookie, `fastboot boot out/xnu-boot.img`, waits for the
phone to reset to TWRP, pulls ramoops to `logs/daily-<time>.log`. While it runs, watch the
live USB log (`bash tools/usb-console-watch.sh 7200`) or `screen /dev/cu.usbmodem* 115200`.
`DEADLINE` is the only escape from a hung boot (every image must carry it); default is 7200s (2 hours) or 1800s for a 30-minute test run.
Phone back in TWRP + `adb devices` = round over.

### 5.1 Required Boot Arguments for Successful Run

The default command line is configured in `tools/build-daily.sh`:
```text
-v cpus=4 keepsyms=1 op2_deadline=7200 rd=emmcroot0 emmcroot=userdata rootrw=1 emmcrw=1 cs_enforcement_disable=1 launchd_unsecure_cache=1 op2fb_client_ui=1 op2_autoinput=0 op2fb_keepon=1 op2_userlog=0 metal=3 op2fb_hwscale=0
```

If you construct a custom boot image or boot via fastboot directly without `tools/build-daily.sh`, the following boot arguments are **required** for a stable, interactive run:

| Boot Argument | Default in Script | Critical? | Purpose & Impact |
| :--- | :--- | :--- | :--- |
| `op2fb_hwscale=0` | Yes | **YES** | **Bypasses Qualcomm QSEED hardware scaler.** Downscaling from Retina 1242×2208 to native 1080×1920 is performed by XNU's fixed-point SIMD bilinear downsampler into carveout slot 0 (`0x03400000`). Setting this to 0 prevents Qualcomm MDSS MDP5 hardware AXI bus underflows and fatal SMMU page faults. |
| `op2fb_client_ui=1` | Yes | **YES** | Enables client UI fallback compositing so SpringBoard surfaces are reliably copied to scanout. |
| `metal=3` | Yes | **YES** | Enables Level 3 Metal GPU acceleration via `OP2Metal` + Adreno 430 Mesa freedreno driver for backboardd, SpringBoard, and apps. |
| `op2_autoinput=0` | Yes | **YES** | Hands-on physical touch mode: synthetic micro-events are disabled completely so that all touch events on the Synaptics panel go directly to iOS UIKit views and dock icons without interference. Screen wake is maintained via `op2fb_keepon=1`. |
| `op2fb_keepon=1` | Yes | **YES** | Keeps the physical display panel and backlight awake at all times. |
| `op2_deadline=7200` | Yes | **YES** | Safety deadline watchdog timer in seconds (e.g. 7200 for 2-hour soak, 1800 for 30m). Automatically syncs filesystems and resets to TWRP recovery upon expiration, preventing hard-lock or battery drain. |
| `rd=emmcroot0` | Yes | **YES** | Directs XNU root filesystem to Qualcomm SDHCI eMMC block device `/dev/emmcroot0`. |
| `emmcroot=userdata` | Yes | **YES** | Selects the internal `userdata` partition as the HFS+ root filesystem. |
| `rootrw=1 emmcrw=1` | Yes | **YES** | Grants read-write access to eMMC and mounts root filesystem read-write. |
| `cs_enforcement_disable=1` | Yes | **YES** | Disables Darwin code signing enforcement, allowing custom userland daemons and shims to execute. |
| `launchd_unsecure_cache=1` | Yes | **YES** | Allows iOS 12 launchd to load patched `xpcd_cache.dylib` services without security cache rejection. |
| `cpus=4` | Yes | **YES** | Boots all 4 Cortex-A53 cores on the power-efficient cluster via SCM mailbox bootstrap. |
| `-v` | Yes | Optional | Verbose kernel boot logging to console. |
| `keepsyms=1` | Yes | Optional | Retains kernel symbol table in RAM for stack backtraces. |
| `op2_userlog=0` | Yes | Optional | Suppresses noisy userland logs from wrapping the 260 KB ramoops ring buffer. |

## 6. Bring-up map (verify each stage before moving on)

1. **USB serial** (`docs/USB-CONSOLE-PLAN.md` stage 1): Mac lists "OP2 USB console"
   (allow the accessory when prompted); live kernel log streams.
2. **Root shell** (stage 2): `op2#` prompt; `isatty` works because the driver's `d_type`
   is `D_TTY` (iOS 12 `isatty` checks `FIODTYPE`, not `TIOCGETA`); job control on.
   Toybox applets live in `/usr/local/bin` (+ `/bin` links for ssh).
3. **Ethernet** (stage 3): Mac gains a USB-Ethernet adapter (MAC `02:00:5e:77:ee:01`);
   set `192.168.7.1/24` on it (`sudo ifconfig enX 192.168.7.1 netmask 255.255.255.0`);
   `ping 192.168.7.2` (phone `ue0`, MAC `..:02`).
4. **SSH** (stage 3): `ssh root@192.168.7.2` (pubkey, host keys auto-created on the
   phone by `dropbear -R`); `scp -O` both ways (legacy protocol: no sftp-server on device).
   Server log: `/tmp/dropbear.log` on the phone.

## 7. Troubleshooting

- **No `/dev/cu.usbmodem*`**: click Allow on the macOS accessory prompt (reappears when the
  USB identity changes); check `ioreg -p IOUSB`. A stale node can linger after a reset —
  unplug/replug if `screen` says busy on a dead device.
- **Ramoops wraps at ~260 KB**: early lines die first; important state is reprinted late
  (periodic `OP2 USB:` lines, deadline summary).
- **Kernel log paused while the shell holds `/dev/op2usb`**: by design (ring drains to the shell).
- **`setup` lines stop at 40**: intentional cap; the periodic lines carry on.
- **Phone hot**: space rounds out (see `CLAUDE.md` heat rule); Low Power Mode helps.

## 8. Current state (2026-11-15)

- **Display & Panel Scanout:** Native MDSS MDP5 command-mode panel refreshing at 60 Hz with zero dropped kicks. SIMD subpixel bilinear downsampling (1242×2208 → 1080×1920 in ~2 ms) bypasses Qualcomm QSEED hardware downscaler (`op2fb_hwscale=0`), completely eliminating physical display freezes and AXI bus faults.
- **Interactive UI & System Apps:** SpringBoard Lock Screen and Home Screen are fully interactive and hands-on (`op2_autoinput=1` sends only 1 pulse to wake display, then leaves 100% control to user). Virtual Home strip is sized to center-third (`x: 420..660, y >= 1885`) directly above the physical ceramic sensor so all 4 dock icons can be clicked and tapped cleanly. System applications launch and navigate cleanly: Settings (`Preferences.app`), Photos (`MobileSlideShow.app`), Camera (`Camera.app`), Clock (`MobileTimer.app`), Safari (`MobileSafari.app`), Health (`Health.app`), and 2048 Game (`Game2048.app`).
- **Native Games:** Playable UIKit 2048 game (`/Applications/Game2048.app`) with full 4-way swipe gestures and tactile on-screen arrow buttons (▲, ▼, ◀, ▶) for responsive gameplay.
- **GPU (Adreno 430):** Full Level 3 Metal acceleration (`metal=3`) active across backboardd, SpringBoard, and userland applications via `OP2Metal` + Mesa freedreno driver.
- **Input & Power:** Synaptics S3320 touch digitizer, capacitive Home button (`OP2Buttons` Consumer HID 0x0C / 0x40), and battery fuel-gauge power telemetry (`OP2Battery` via Qualcomm PMI8994 SPMI).
- **USB & Networking:** Composite CDC-ACM serial console + CDC-ECM Ethernet (`ue0` 192.168.7.2); Dropbear SSH server with pubkey authentication and `scp -O` file transfer.
