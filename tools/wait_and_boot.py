#!/usr/bin/env python3
import subprocess
import time
import sys

SERIAL = "ebd7ee57"
BOOT_IMG = "out/xnu-boot.img"

print(f"[WAIT_AND_BOOT] Waiting for device {SERIAL} to reset into TWRP recovery or fastboot...", flush=True)

start_time = time.time()
last_report = start_time
while True:
    now = time.time()
    if now - last_report > 15:
        elapsed = int(now - start_time)
        print(f"[WAIT_AND_BOOT] Still waiting... ({elapsed}s elapsed)", flush=True)
        last_report = now

    # Check adb
    try:
        res = subprocess.run(["adb", "devices"], capture_output=True, text=True, timeout=1)
        for line in res.stdout.splitlines():
            if SERIAL in line and "recovery" in line:
                print(f"\n[WAIT_AND_BOOT] Detected in TWRP recovery! Re-arming BCB cookie...", flush=True)
                cmd_arm = ["adb", "-s", SERIAL, "shell", "printf 'boot-recovery\\0' | dd of=/dev/block/bootdevice/by-name/misc bs=14 count=1 conv=notrunc"]
                subprocess.run(cmd_arm, capture_output=True)
                time.sleep(1)
                print(f"[WAIT_AND_BOOT] Rebooting to bootloader...", flush=True)
                subprocess.run(["adb", "-s", SERIAL, "reboot", "bootloader"], capture_output=True)
                # Wait for fastboot
                for _ in range(30):
                    time.sleep(1)
                    fb = subprocess.run(["fastboot", "devices"], capture_output=True, text=True)
                    if SERIAL in fb.stdout:
                        break
                print(f"[WAIT_AND_BOOT] Fastboot ready. RAM-booting {BOOT_IMG}...", flush=True)
                boot_res = subprocess.run(["fastboot", "-s", SERIAL, "boot", BOOT_IMG], capture_output=True, text=True)
                print(f"[WAIT_AND_BOOT] Boot output:\n{boot_res.stdout}\n{boot_res.stderr}", flush=True)
                sys.exit(0)
    except Exception:
        pass

    # Check fastboot directly
    try:
        fb = subprocess.run(["fastboot", "devices"], capture_output=True, text=True, timeout=1)
        if SERIAL in fb.stdout:
            print(f"\n[WAIT_AND_BOOT] Detected directly in fastboot! RAM-booting {BOOT_IMG}...", flush=True)
            boot_res = subprocess.run(["fastboot", "-s", SERIAL, "boot", BOOT_IMG], capture_output=True, text=True)
            print(f"[WAIT_AND_BOOT] Boot output:\n{boot_res.stdout}\n{boot_res.stderr}", flush=True)
            sys.exit(0)
    except Exception:
        pass

    time.sleep(1)
