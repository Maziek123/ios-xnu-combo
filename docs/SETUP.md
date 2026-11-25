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
| `userland/` | yes | `op2shell` (launcher), `op2gpu`, `OP2Metal`, `op2mtl`, `op2bench`, `games/2048` sources |
| `docs/` | yes | plans, status, this guide |
| `kernel-backup/` | yes | before/after copies of kernel files changed per round (`<round>/pre/` = before) |
| `xnu/xnu-4903.270.47` | own repo ([Veerhooda/xnu](https://github.com/Veerhooda/xnu)) | the kernel; gitignored here |
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

## 4. Build order (fresh checkout → the image we run today)

0. Kernel source: `git clone https://github.com/Veerhooda/xnu xnu/xnu-4903.270.47` (branch `main`; `msm8994`
   is the same commit). It is gitignored here because it is its own repository.
1. Kernel + boot image: `bash tools/build-daily.sh` → `out/xnu-boot.img` (refuses if the
   cmdline would exceed 250 bytes).
2. Shell tools (each stages into `out/shell/build/`, stripped + ad-hoc signed):
   `bash tools/build-toybox.sh`, `bash tools/build-dropbear.sh`,
   then `bash tools/build-op2shell.sh` (needs dash + toybox + dropbear + scp +
   `out/shell/build/authorized_keys`, i.e. your `.pub`).
3. GPU userland: `bash tools/mesa-host.sh`, `bash tools/mesa-ios.sh` (→ `out/mesa-ios/out/op2mtl.dylib`),
   `bash userland/OP2Metal/build.sh` (→ `userland/OP2Metal/build/OP2Metal.bundle`). Without these the phone
   still boots, but CoreAnimation renders on the CPU (`metal=3` needs both).
4. Root filesystem, phone in TWRP: `bash tools/rootfs-journal.sh`. From `out/ios12-userdata-fixA.dmg` it makes a
   journaled copy and installs, in order: accessibility prefs (`rootfs_prefs.py`), the G3 test binary if built
   (`rootfs_g3.py`), `op2mtl.dylib` + `OP2Metal` in growable slots (`rootfs_g5.py`), every
   `out/games/build/*.app` (`rootfs_games.py`), and the USB shell/SSH launcher with its launchd job
   (`rootfs_shell.py`). Then it streams the image to `userdata` and arms the recovery cookie.
   This erases the phone's iOS userdata.
5. `bash tools/round.sh` (section 5).

Later updates go in place, without rebuilding the rootfs: `bash tools/build-op2shell.sh --install` for the
launcher, `tools/op2hfs.py write <path> 0 <file>` for anything with a slot (OP2Metal, op2mtl).
`op2hfs.py` grows a file only up to the blocks already allocated to it.

Do not add jobs or dylibs to SpringBoard. A 2026-11 experiment that injected code into SpringBoard through the
launchd cache (`DYLD_INSERT_LIBRARIES`) slowed the UI and locked the passcode; it was removed. The clean
SpringBoard and job cache are what `rootfs-journal.sh` produces.

## 5. The round loop (ephemeral boot, never flash)

```bash
bash tools/round.sh                 # 15 min (DEADLINE=900)
DEADLINE=1800 bash tools/round.sh   # longer
```

Builds, re-arms the BCB recovery cookie, `fastboot boot out/xnu-boot.img`, waits for the
phone to reset to TWRP, pulls ramoops to `logs/daily-<time>.log`. While it runs, watch the
live USB log (`bash tools/usb-console-watch.sh 900`) or `screen /dev/cu.usbmodem* 115200`.
`DEADLINE` (boot-arg `op2_deadline`) is the only escape from a hung boot; every image must carry it.
`round.sh` defaults to 900 s; `build-daily.sh` alone defaults to 7200 s.
Phone back in TWRP + `adb devices` = round over.

### 5.1 Boot arguments

All of them are set in `tools/build-daily.sh` (`EXTRA="..."` appends, `AUTOIN=` sets `op2_autoinput`):
```text
-v cpus=4 keepsyms=1 op2_deadline=900 rd=emmcroot0 emmcroot=userdata rootrw=1 emmcrw=1 cs_enforcement_disable=1 launchd_unsecure_cache=1 op2fb_client_ui=0 op2_autoinput=0 op2fb_keepon=1 op2_userlog=0 metal=3 op2fb_hwscale=0
```
XNU truncates the command line at 255 bytes; the script refuses at 250.

| Boot-arg | Value | What it does |
| :--- | :--- | :--- |
| `op2_deadline` | seconds | Syncs the filesystems and resets into TWRP after this long. Required. |
| `rd=emmcroot0 emmcroot=userdata` | | Root filesystem: the HFS+ image on the `userdata` partition. |
| `rootrw=1 emmcrw=1` | | Mount root read-write; allow eMMC writes. |
| `cs_enforcement_disable=1` | | Lets our ad-hoc-signed binaries run. |
| `launchd_unsecure_cache=1` | | launchd accepts the job cache we edited (`xpcd-cache-add.py`). |
| `cpus=4` | | Brings up all four A53 cores. |
| `metal=3` | 0–3 | 3: CoreAnimation renders through OP2Metal on the Adreno. 0: CPU rendering. 2: function-constant capture (blank screen). |
| `op2fb_hwscale` | 0 | 0: CPU downsample. 1: MDP5 hardware scaler (needs the source inside the LK carveout; not used). |
| `op2fb_double` | 1 (default) | Double-buffered panel. 0 makes the copy write the buffer the panel is reading. |
| `op2fb_client_ui` | 0 | 1 makes the framebuffer scan client surfaces itself; it was part of the slow path, keep 0. |
| `op2fb_swapwait` | 1 (default) | SwapWait blocks the render server until its frame is copied (50 ms cap). |
| `op2_autoinput` | 0 | 0: no synthetic input. 1: one wake tap. Higher values are old scripted tests; do not use them to claim anything works. |
| `op2fb_keepon=1` | | Keeps the panel and backlight on. |
| `op2_userlog=0` | | Keeps userland logs out of the 260 KB ramoops ring. |
| `-v keepsyms=1` | | Verbose boot, symbols kept for backtraces. |

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
5. **Internet over USB**: the phone publishes `ue0` to configd itself (op2shell) with the Mac as router and
   1.1.1.1/8.8.8.8 as DNS. The Mac must forward and NAT (both reset when the Mac reboots):
   ```bash
   sudo sysctl -w net.inet.ip.forwarding=1
   echo "nat on en0 inet from 192.168.7.0/24 to any -> (en0)" | sudo pfctl -a com.apple/op2nat -f - && sudo pfctl -E
   ```
   (`en0` = the Mac's internet interface: `route -n get default`.) Check on the phone:
   `/usr/local/bin/op2net example.com` → `http HTTP/1.1 200 OK`; `scutil --nwi` shows `ue0` reachable.
   If `scutil` shows no `Plugin:`/`Setup:` keys at all, configd's plugins are stuck: the kernel must publish
   `IOPlatformUUID` (`OP2: IOPlatformUUID` in the boot log).

## 7. Troubleshooting

- **No `/dev/cu.usbmodem*`**: click Allow on the macOS accessory prompt (reappears when the
  USB identity changes); check `ioreg -p IOUSB`. A stale node can linger after a reset —
  unplug/replug if `screen` says busy on a dead device.
- **Ramoops wraps at ~260 KB**: early lines die first; important state is reprinted late
  (periodic `OP2 USB:` lines, deadline summary).
- **Kernel log paused while the shell holds `/dev/op2usb`**: by design (ring drains to the shell).
- **`setup` lines stop at 40**: intentional cap; the periodic lines carry on.
- **Phone hot**: space rounds out (see `CLAUDE.md` heat rule); Low Power Mode helps.

## 8. Current state (2026-11-17)

Boots to the Lock Screen and Home Screen, unlocks, touch and Home work, the UI is composited on the Adreno
(`metal=3`). ~8 ms per present, SwapWait timeouts 0, ~1 % kernel CPU when idle, ~61 °C at 768 MHz.
Serial shell + SSH over USB work. Open: brief partial frames while scrolling (being re-checked with double
buffering back on), internet over USB works through the Mac's NAT (Wi-Fi: joins a WPA2 network since 2026-11-25 but is not a network interface for iOS yet, see README), no RTC, the crash loops listed in
`CLAUDE.md`. Next work: `docs/NEXT-PLAN.md`.
