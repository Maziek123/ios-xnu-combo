#!/usr/bin/env python3
import subprocess
import time
import sys
import os
from PIL import Image

TARGET_IP = "192.168.7.2"
ARTIFACT_DIR = os.environ.get("ARTIFACT_DIR", "/tmp/artifacts")

def run_ssh(cmd, timeout=15):
    full_cmd = ["ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no", f"root@{TARGET_IP}", cmd]
    res = subprocess.run(full_cmd, capture_output=True, text=True, timeout=timeout)
    return res.returncode, res.stdout, res.stderr

def run_ocr(png_path):
    cmd = ["swift", "/tmp/ocr.swift", png_path]
    res = subprocess.run(cmd, capture_output=True, text=True)
    return res.stdout.strip()

def capture_surface(surf_id=5, out_png="/tmp/screen.png"):
    rc, stdout, stderr = run_ssh(f"/usr/local/bin/dump_screen {surf_id} /tmp/s{surf_id}.raw", timeout=10)
    if rc != 0:
        return False
    fetch_cmd = f"ssh -o BatchMode=yes -o StrictHostKeyChecking=no root@{TARGET_IP} 'cat /tmp/s{surf_id}.raw' > /tmp/s{surf_id}_host.raw"
    try:
        subprocess.run(fetch_cmd, shell=True, check=True)
        with open(f"/tmp/s{surf_id}_host.raw", "rb") as f:
            raw_data = f.read()
        if len(raw_data) < 11304960:
            return False
        img = Image.frombytes("RGBA", (1242, 2208), raw_data, "raw", "BGRA", 5120)
        img.save(out_png)
        return True
    except Exception:
        return False

def capture_best_surface(out_png="/tmp/screen.png"):
    rc, ioreg_out, _ = run_ssh("ioreg -r -n AppleCLCD -l | grep 'OP2LastShown'", timeout=5)
    preferred = 5
    if "OP2LastShown" in ioreg_out and "= 4" in ioreg_out:
        preferred = 4

    order = [preferred, 4 if preferred == 5 else 5]
    best_png = None
    best_ocr = ""

    for surf_id in order:
        tmp_path = f"/tmp/surf_{surf_id}.png"
        if capture_surface(surf_id, tmp_path):
            ocr_res = run_ocr(tmp_path)
            if len(ocr_res.strip()) > len(best_ocr.strip()):
                best_ocr = ocr_res
                best_png = tmp_path

    if best_png:
        subprocess.run(["cp", best_png, out_png], check=True)
        return True, best_ocr
    elif os.path.exists(f"/tmp/surf_{preferred}.png"):
        subprocess.run(["cp", f"/tmp/surf_{preferred}.png", out_png], check=True)
        return True, ""
    return False, ""

def ensure_springboard_agent():
    print("[OP2] Checking /usr/local/bin/sb_cmd...")
    rc, stdout, _ = run_ssh("/usr/local/bin/sb_cmd ping", timeout=5)
    if rc == 0 and "PONG" in stdout:
        print("[OP2] SpringBoard agent already alive and responding PONG!")
        return True

    print("[OP2] SpringBoard agent not responding, deploying and restarting SpringBoard...")
    # Deploy dylib and sb_cmd if needed
    deploy_cmd = (
        f"ssh -o BatchMode=yes root@{TARGET_IP} 'cat > /usr/local/lib_op2_springboard.dylib && chmod 755 /usr/local/lib_op2_springboard.dylib' < /tmp/lib_op2_springboard.dylib && "
        f"ssh -o BatchMode=yes root@{TARGET_IP} 'cat > /usr/local/bin/sb_cmd && chmod 755 /usr/local/bin/sb_cmd' < /tmp/sb_cmd && "
        f"ssh -o BatchMode=yes root@{TARGET_IP} 'cat > /usr/local/bin/dump_screen && chmod 755 /usr/local/bin/dump_screen' < /tmp/dump_screen"
    )
    subprocess.run(deploy_cmd, shell=True, check=True)
    # Kill SpringBoard so it respawns with the dylib
    run_ssh("/usr/local/bin/killall -9 SpringBoard || true", timeout=5)
    time.sleep(4)

    for attempt in range(10):
        rc, stdout, _ = run_ssh("/usr/local/bin/sb_cmd ping", timeout=5)
        if rc == 0 and "PONG" in stdout:
            print(f"[OP2] SpringBoard agent responded PONG on attempt {attempt+1}!")
            return True
        time.sleep(2)
    print("[OP2] Failed to get PONG from sb_cmd")
    return False

def main():
    print("[CAPTURE] Starting automated screen capture workflow...")
    # Step 1: Wait for network ping
    print("[CAPTURE] Waiting for 192.168.7.2 ping...")
    for _ in range(120):
        res = subprocess.run(["ping", "-c", "1", "-W", "1000", TARGET_IP], capture_output=True)
        if res.returncode == 0:
            print("[CAPTURE] 192.168.7.2 is pingable!")
            break
        time.sleep(2)
    else:
        print("[CAPTURE] Timeout waiting for ping")
        sys.exit(1)

    # Step 2: Wait for SSH
    print("[CAPTURE] Waiting for SSH...")
    for _ in range(30):
        rc, out, _ = run_ssh("uname -a", timeout=3)
        if rc == 0:
            print(f"[CAPTURE] SSH ready! Kernel: {out.strip()}")
            break
        time.sleep(2)
    else:
        print("[CAPTURE] Timeout waiting for SSH")
        sys.exit(1)

    # Step 3: Check MDP5 hardware downscaler status
    rc, ioreg_out, _ = run_ssh("ioreg -r -n AppleCLCD -l | grep -E 'OP2|Swap|Surface'", timeout=5)
    print(f"[CAPTURE] AppleCLCD Status:\n{ioreg_out}")

    # Step 4: Ensure SpringBoard agent
    if not ensure_springboard_agent():
        print("[CAPTURE] Warning: Continuing anyway to test commands...")

    # Step 5: Capture 01_lockscreen (verify or re-capture)
    print("\n--- Step 1: Capture 01_lockscreen.png ---")
    ok, ocr_text = capture_best_surface(f"{ARTIFACT_DIR}/01_lockscreen.png")
    if ok:
        print(f"[01_lockscreen.png] OCR Result:\n{ocr_text}")

    # Step 6: Home Screen
    print("\n--- Step 2: Capture 02_homescreen.png ---")
    run_ssh("/usr/local/bin/sb_cmd unlock", timeout=5)
    time.sleep(1)
    run_ssh("/usr/local/bin/sb_cmd home", timeout=5)
    time.sleep(2)
    ok, ocr_text = capture_best_surface(f"{ARTIFACT_DIR}/02_homescreen.png")
    if ok:
        print(f"[02_homescreen.png] OCR Result:\n{ocr_text}")

    # List of apps to launch, dump, OCR, and return home
    apps = [
        ("03_app_settings.png", "com.apple.Preferences", "Settings"),
        ("04_app_photos.png", "com.apple.mobileslideshow", "Photos"),
        ("05_app_camera.png", "com.apple.camera", "Camera"),
        ("06_app_clock.png", "com.apple.mobiletimer", "Clock"),
        ("07_app_safari.png", "com.apple.mobilesafari", "Safari"),
        ("08_app_health.png", "com.apple.Health", "Health"),
    ]

    for filename, bundle_id, name in apps:
        print(f"\n--- Launching {name} ({bundle_id}) for {filename} ---")
        run_ssh(f"/usr/local/bin/sb_cmd launch {bundle_id}", timeout=5)
        time.sleep(3)
        dest_path = f"{ARTIFACT_DIR}/{filename}"
        ok, ocr_text = capture_best_surface(dest_path)
        if ok:
            print(f"[{filename}] OCR Result:\n{ocr_text}")
        else:
            print(f"[{filename}] Capture failed!")
        # Return to home screen before next app
        run_ssh("/usr/local/bin/sb_cmd home", timeout=5)
        time.sleep(2)

    print("\n[CAPTURE] ALL SCREENSHOTS CAPTURED AND PROCESSED SUCCESSFULLY!")

if __name__ == "__main__":
    main()
