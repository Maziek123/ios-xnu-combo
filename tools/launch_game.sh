#!/usr/bin/env bash
# launch_game.sh - Wait for device, deploy latest SpringBoard dylib, unlock, and launch 2048 game
set -euo pipefail

TARGET_IP="192.168.7.2"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "[*] Checking device connection..."
until ssh -o BatchMode=yes -o ConnectTimeout=2 "root@${TARGET_IP}" 'uname -a' >/dev/null 2>&1; do
    echo "[-] Waiting for iOS SSH at ${TARGET_IP}..."
    sleep 2
done

echo "[+] iOS is online!"
echo "[*] Deploying latest lib_op2_springboard.dylib..."
ssh -o BatchMode=yes "root@${TARGET_IP}" 'cat > /usr/local/lib_op2_springboard.dylib && chmod 755 /usr/local/lib_op2_springboard.dylib' < "${REPO_ROOT}/out/springboard/lib_op2_springboard.dylib"

echo "[*] Respawning SpringBoard to apply new dylib..."
SB_PID=$(ssh -o BatchMode=yes "root@${TARGET_IP}" 'ps aux | grep SpringBoard | grep -v grep | head -n 1' | awk '{print $2}' || true)
if [ -n "${SB_PID}" ]; then
    ssh -o BatchMode=yes "root@${TARGET_IP}" "kill -9 ${SB_PID}" || true
fi

echo "[*] Waiting for SpringBoard to initialize..."
sleep 6

echo "[*] Unlocking and launching 2048 game..."
ssh -o BatchMode=yes "root@${TARGET_IP}" '/usr/local/bin/sb_cmd unlock && sleep 1 && /usr/local/bin/sb_cmd game'

echo "[+] 2048 game launched and ready for touch input!"
