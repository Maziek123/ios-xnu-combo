# OnePlus 2 iOS port: context for Claude Code

Read first: docs/AUDIT-2026-11-11.md (what was wrong + fixes), docs/GPU-PLAN.md (next work).
docs/HANDOVER_KNOWLEDGE_BASE.md is the PREVIOUS agent's handover: useful facts, but several
claims were wrong ("0 crashes", "verified" milestones driven by a scripted fake user).

## Hard safety rules
- Volume/Power buttons are broken **on our unit** — never rely on them here. A unit with working
  buttons is fine (they behave normally); the BCB-cookie + deadline rules below apply either way.
- Never `fastboot flash` anything. Boot only with `fastboot boot out/xnu-boot.img`.
- Keep the BCB cookie armed (tools/boot-daily.sh does it) so every reset lands in TWRP.
- Every boot image must have op2_deadline (the only escape from a hang; HW watchdog strands the phone).
- Don't touch CPU voltage beyond the stock values in op2_cpu_boost (APC0 1.145 V, MX super-turbo).
- Kernel source under xnu/ is gitignored: back up before editing (kernel-backup/), or track with git add -f.

## Loop
    bash tools/round.sh            # build + RAM-boot + wait for reset + save log (DEADLINE=900 default)
    logs/last-summary.txt, logs/daily-*.log   # results
    bash tools/rootfs-journal.sh   # rebuild + flash journaled rootfs from out/ios12-userdata-fixA.dmg
Build errors land in logs/last-build-errors.txt. kprintf does NOT reach ramoops (no serial=3);
use printf/IOLog for anything you need to see. Kept log (bsd/dev/op2_keeplog.c): op2_keep(fmt, ...) / sysctl
kern.op2keep put important lines in ramoops record zones 0-63 (TWRP layout read 2026-11-27: 1 MB at 0xac000000, 190 x 4 KB
records, console 256 KB at 0xac0be000, ftrace 4 KB, pmsg 4 KB; outside iOS's RAM); the kernel writes the DBGC headers itself
(none survive to iOS: the first version found none and kept nothing), safe from console floods; TWRP:
/sys/fs/pstore/dmesg-ramoops-0, -1, ... in order (README "Testing a step"). Wi-Fi A_LOG/W_LOG, op2_wlan,
OP2 SHELL and op2netd lines go there; op2keep=0 off. Boot-arg op2quiet (default ON, sleh.c op2_quiet()): NECP
delegation refusals (dasd) and code-signing "allowing" page notes not logged at all; crashes one PROC_EXIT_CRASH line,
3 per name; op2quiet=0 restores all.

## State (2026-11-17; see docs/SETUP.md for replication, docs/USB-CONSOLE-PLAN.md stages 2-3)
- Quad-core SMP (cpus=4); thermal thread: TSENS, throttle at 70 C, cutoff 90 C (~768 MHz at low 60s C).
- eMMC CMD18 multi-block reads with self-test; 0 failures so far.
- Panel: double-buffered (op2fb_double, default 1 again), SwapWait, immediate present, block downsample
  1242x2208->1080x1920 ~6 ms, ~8 ms/present. A frame that arrives inside the 15.6 ms kick spacing is copied
  at once into the back buffer and only the flip+kick waits (presentPending/flipAndKick, `early=` in FB PERF).
  Surfaces in the uncached carveout: panel-sized (1080x1920) only - 1242x2208 there made the copy ~183 ms.
- Home = touch starting on bottom strip (centre third), consumed; sent once via OP2Buttons.
- No synthetic input (op2_autoinput=0). op2fb_client_ui=0. Rootfs journaled, synced before deadline.
- USB serial console (CDC-ACM, live kernel log) + root dash shell on /dev/op2usb with job control
  (d_type D_TTY: iOS 12 isatty checks FIODTYPE, not TIOCGETA) + toybox applets (/usr/local/bin and /bin).
- Composite CDC-ECM: phone ue0 192.168.7.2/24, Mac 192.168.7.1/24, ping verified; SSH exec + scp -O +
  pubkey auth verified (dropbear, host keys via -R, /tmp/dropbear.log on phone). SSH PATH lacks
  /usr/local/bin: use full paths. No sleep/sort/chown on the phone (/usr/local/bin/op2chown exists).
- GPU G0-G3 pass; G4d/G4e SMMU + pvtmem fix; G5 metal=3 composites home/lock/dock on the Adreno
  (see docs/GPU-PLAN.md). OrangeTree game still blank (SpriteKit pipelines return NULL in the shim) - open.
- Internet over USB works (2026-11-17): kernel publishes IOPlatformUUID (OP2PlatformExpert; without it
  gethostuuid() blocked configd's IPConfiguration plugin forever, so NO network plugin ever ran). op2shell
  publishes service op2usb (State:/Network/Service/op2usb/IPv4+DNS, router 192.168.7.1, DNS 1.1.1.1/8.8.8.8;
  re-published every 15 s if configd loses it). Mac must NAT: pfctl anchor com.apple/op2nat (docs/SETUP.md 6).
  Test: /usr/local/bin/op2net example.com.
- Wi-Fi (2026-11-26): QCA6174 on PCIe, ath10k ported in iokit/Kernel/arm/OP2Wifi.cpp + OP2Ath10k.cpp, boot-arg wifi=N
  (each N runs all lower steps; README "Wi-Fi bring-up"). wifi=9 joins a WPA2 hotspot named in the gitignored
  firmware/qca6174/wifi-network.txt (tools/gen-wifi-fw.py -> op2_wifi_net.h: name + PMK only) and runs the 4-way
  handshake (STEP 6 PASS on the phone); wifi=10 adds DHCP/ARP/ping through the keys (STEP 7 PASS). wifi=11 (step 8,
  2026-11-26, PASS on the phone: Safari loaded google.com over Wi-Fi, ~1 MB in the first minute): wl0 ifnet in bsd/dev/op2_wlan.c, sysctl net.op2wlan = lease, op2shell
  publishes service op2wifi ahead of op2usb; so does op2netd (userland/op2netd, same publish code in
  userland/op2shell/op2_publish.h): build-xnu.sh embeds it (and op2shell if built) in the kernel, bsd/kern/op2_shell_sync.c
  writes /usr/sbin/op2netd before launchd and exec_save_path() runs it in place of launchd's /usr/sbin/wifid (it then
  execs the real wifid); op2netd=0 / op2shell_sync=0 off; step 8 logs the state (user's phone: likely no op2shell);
  stays connected, polled loop, 16 tx slots, status every 60 s. Renumbered 2026-11-25: one number per step.
  wifi=12 (2026-11-26): Wi-Fi manager after step 4 (OP2Ath10k.cpp run_manager; gWant* is the
  network, from op2_wifi_net.h first, only if step 4 heard it, else skipped for the boot; then op2netd's "join"); op2netd serves http://127.0.0.1:8080 (list, join with
  password -> PMK via CommonCrypto, saved networks in /var/root/Library/op2netd/networks, autojoin, leave, forget, off)
  over sysctls net.op2wlan_cmd/_scan/_state. Tests: wifi-sim scenarios 35-37, tools/op2netd-test/run.sh (host, curl).
  wifi=12 PASSES on the phone (2026-11-26: home router from the page, Safari, fast.com 8/21 Mbit/s, 11g rates). Page: saved
  networks join on tap, "..." menu (Advanced, change password, forget), connected network's details dropdown; Advanced =
  per-network settings in the networks file (auto=0 ip= mask= router= dns=a,b,c dnsauto=0 search=): manual IPv4 rides on
  "join ... static <ip> <mask> <router> <dns>" (8 hex each; step 7 skips DHCP), DNS/search go to configd via
  op2_publish.h's publish_state.dns_list/search (op2shell leaves them empty = old behaviour). Page settings work on the
  phone (user's report, README progress row 24). Built, not run yet: wifid trace (666c8a0) + Bluetooth step 1 (5c3a7fe).
  Status-bar Wi-Fi icon / Settings Wi-Fi (plan: make Apple's wifid see a Wi-Fi device, never patch SpringBoard): S0 =
  trace what wifid asks for, kept log lines "OP2 WIFID:" (boot-arg op2wifid=0 off): its IOKit matching, notifications,
  registry paths and opens (IOUserClient.cpp op2_iolog, own 150-line budget) and each distinct ioctl with interface
  name + Apple80211 request words (sys_generic.c op2_wifid_ioctl, 120 max).
  Bluetooth (2026-11-26, not run yet): OP2Wifi.cpp, boot-arg bt=1 probe / bt=2 step 1 (out of reset, chip seen on the
  UART pins), run before Wi-Fi in the same thread, "OP2 BT:" lines. UART = BLSP2 UART2 0xf995e000 (ttyHS0), pins 45-48
  TX/RX/CTS/RFR func 2, GCC blsp2 ahb 0x0944 (vote 0x1484 bit 15), uart2 apps CBCR 0x0a44, RCG 0x0a4c; reset PM8994
  GPIO 19 (0xd200, mode 0x10 low / 0x11 high, vin 2). Plan: own HCI driver + op2netd page; iOS bluetoothd not used.
  The clock now syncs over the network (NTP), which fixes the old TLS-date problem while connected.
  Firmware blobs + generated headers stay out of git. Before a phone round: bash tools/wifi-sim/run.sh (simulated chip + OpenSSL access point, 38 scenarios)
  and tools/wifi-sim/kcheck.sh (XNU warning flags).
- Safari (2026-11-17): loads/draws normal sites (google, wikipedia) after kernel NECP ADD_FLOW, entitlement
  privileges (kern_priv), AppleM2ScalerCSCDriver stub; WebContent runs on the CPU (OP2Metal: one op2mtl context
  deadlocked its nested passes). JavaScript fixed 2026-11-17: Safari's +[NSUserDefaults _sf_safariDefaults] needs
  sandbox_container_path_for_pid (__mac_syscall "Sandbox" call 4); with no Sandbox kext it failed and the fallback
  gave nil defaults -> every web view had JavaScript off. mac_base.c now answers call 4 for the caller itself
  from its HOME (an app's container). YouTube/apple.com load (rough). Found by op2inject+op2probe (live Safari).
- Ownership bug (open): only the root cfprefsd runs (no per-user agent), and containermanagerd's containers are
  root-owned, so mobile's prefs/containers come out root:wheel 0600. Repaired by hand on the phone 2026-11-17
  (op2chown -R 501 501 on Containers/Data/Application, Data/PluginKitPlugin, Shared/AppGroup + 108 prefs);
  new files will be root-owned again until the cause is fixed. Clock ~4 days behind (no RTC): new TLS certs fail.
- Media / YouTube: VIDEO PLAYS (2026-11-17): youtube.com videos play in Safari (silent; "Me at the zoo" ran to the
  end, test clips loop). Chain: AVPlayer -> mediaserverd (remote VideoToolbox) -> userland/op2video libop2h264.dylib
  (FFmpeg 6.1.2 H.264, LGPL, patched for decode-order output: patches/h264-decode-order.patch; VTRegisterVideoDecoder
  'avc1', loaded by OP2NullAudio's constructor) -> NV12 IOSurfaces -> backboardd: CA asks for MTLPixelFormat 500
  (two-plane YCbCr, AGX-native) which OP2Metal emulates as BGRA converted on the CPU per new frame (IOSurface seed),
  bound via the private setFragmentTexture:atTextureIndex:samplerState:atSamplerIndex:. Kernel fixes on the way:
  IOSurface plane record layout (offset 0x0c, bpr 0x10, size 0x14), IOSurface use-count methods 14/15 (+5/11/20/22/26
  answered), AppleCLCD DisplayPipePlaneBaseAlignment must be a dictionary {PlaneBaseAlignmentLinear}. mediaserverd
  runs (OP2NullAudio silent device + VirtualAudio; blocking HAL plug-ins in HAL.op2-off; tools/rootfs_audio.py).
  Open: heavy pages can turn the whole screen white = GPU pool (512 MB) exhausted -> surfaces off-pool -> GPU fence
  timeouts (OP2 GPU POOL lines now log free/largest/extents); real audio (Qualcomm WCD9330); YUV->RGB is scalar C.
- Driving the phone without hands: op2home N (Home presses: wake+unlock, no passcode), op2open URL (Safari etc.),
  op2fbshot OUT.png (panel scanout via AppleCLCD debug selector 96), test pages from a Range-capable server.
- Phone tools (userland/op2tools): op2stack PID (thread backtraces, image+offset), op2net, op2url (NSURLSession),
  op2path (Network.framework path), op2js (JavaScriptCore), op2mc (restrictions), op2kill, op2reboot,
  op2inject/op2probe/op2crash (load a dylib into a running process; Safari probe; crash reports), op2tone (audio).
- Open: partial frames while scrolling (recheck with dbl=1), logd SIGBUS/SIGSEGV loop (stack probe),
  Spotlight extensions SIGABRT loop, occasional SpringBoard SIGSEGV, no RTC.
- 2026-11-14..16 another agent injected lib_op2_springboard/lib_op2_app_fix into SpringBoard via the
  xpcd cache, patched SpringBoard, and made the slow bilinear downsampler + 1242x2208 carveout surfaces.
  All reverted 2026-11-17 (phone: stock SpringBoard, xpcd_cache = backup/rootfs/xpcd_cache.dylib.op2shell,
  the two dylibs replaced by an empty one; originals backup/rootfs/*.otherai*). Do not reintroduce.
- Reference for GPU: hakios (MIT) github.com/oosexgang/hakios-xnu-kernel (Metal driver,
  AIR->NIR front end tools/mesa/air). Mesa 25.0.x needed (ir3 for chip 0x04030002).
