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
use printf/IOLog for anything you need to see.

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
  Test: /usr/local/bin/op2net example.com. Wi-Fi (QCA6174 on PCIe) has no driver.
- Safari (2026-11-17): loads/draws normal sites (google, wikipedia) after kernel NECP ADD_FLOW, entitlement
  privileges (kern_priv), AppleM2ScalerCSCDriver stub; WebContent runs on the CPU (OP2Metal: one op2mtl context
  deadlocked its nested passes). OPEN: JavaScript never runs in WebContent (JSC+JIT fine in a test tool,
  Safari switch + MC restriction allowed); heavy sites (YouTube, apple.com) turn the whole screen white.
- Phone tools (userland/op2tools): op2stack PID (thread backtraces, image+offset), op2net, op2url (NSURLSession),
  op2path (Network.framework path), op2js (JavaScriptCore), op2mc (restrictions), op2kill, op2reboot.
- Open: partial frames while scrolling (recheck with dbl=1), logd SIGBUS/SIGSEGV loop (stack probe),
  Spotlight extensions SIGABRT loop, occasional SpringBoard SIGSEGV, no RTC.
- 2026-11-14..16 another agent injected lib_op2_springboard/lib_op2_app_fix into SpringBoard via the
  xpcd cache, patched SpringBoard, and made the slow bilinear downsampler + 1242x2208 carveout surfaces.
  All reverted 2026-11-17 (phone: stock SpringBoard, xpcd_cache = backup/rootfs/xpcd_cache.dylib.op2shell,
  the two dylibs replaced by an empty one; originals backup/rootfs/*.otherai*). Do not reintroduce.
- Reference for GPU: hakios (MIT) github.com/oosexgang/hakios-xnu-kernel (Metal driver,
  AIR->NIR front end tools/mesa/air). Mesa 25.0.x needed (ir3 for chip 0x04030002).
