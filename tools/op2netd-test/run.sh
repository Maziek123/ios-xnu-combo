#!/usr/bin/env bash
# op2netd's Wi-Fi page on this host (Linux: gcc, curl, OpenSSL): the kernel's sysctls are files, the page is driven
# with curl and the commands op2netd sends are checked. bash tools/op2netd-test/run.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
T=$(mktemp -d)
trap 'kill $PID 2>/dev/null || true; rm -rf "$T"' EXIT
gcc -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE -DOP2NETD_TEST_DIR="\"$T\"" -I"$HERE/stub" -o "$T/netd" \
	"$HERE/host.c" -lcrypto
mkdir -p "$T/k"
printf 'idle' > "$T/k/net.op2wlan_state"
printf '82:98:3e:cf:7c:84 6 -47 WPA2 6950686f6e65\n02:ca:fe:00:00:01 11 -72 open 43616665\n10:27:f5:36:80:f8 1 -35 WPA2 3c623e3c2f623e\n10:27:f5:36:80:f9 1 -40 WPA2 -\n' \
	> "$T/k/net.op2wlan_scan"
"$T/netd" > /dev/null 2>&1 &
PID=$!
C=http://127.0.0.1:8080
for _ in 1 2 3 4 5 6 7 8 9 10; do curl -s -o /dev/null $C/ && break; sleep 0.3; done
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
page=$(curl -s $C/)
check "list: iPhone, Cafe, escaped <b></b>, hidden left out" \
	'[[ "$page" == *"/join?n=6950686f6e65\"><span class=\"name\">iPhone"* && "$page" == *">Cafe"* && "$page" == *"&lt;b&gt;&lt;/b&gt;"* ]]'
check "password page" '[[ "$(curl -s "$C/join?n=6950686f6e65")" == *"<h1>iPhone</h1>"*"type=\"password\""* ]]'
code=$(curl -s -o /dev/null -w '%{http_code}' -d 'n=6950686f6e65&p=simulated-ap' $C/join)
pmk=$(python3 -c "import hashlib;print(hashlib.pbkdf2_hmac('sha1',b'simulated-ap',b'iPhone',4096,32).hex())")
check "join: 303, PMK = PBKDF2(password, name)" '[[ $code == 303 && "$(tail -1 "$T/k/cmds")" == "join 6950686f6e65 $pmk" ]]'
check "saved 0600" '[[ "$(stat -c %a "$T/state/op2netd/networks")" == 600 && "$(cat "$T/state/op2netd/networks")" == "6950686f6e65 $pmk" ]]'
curl -s -o /dev/null -d 'n=6950686f6e65&p=short' $C/join
check "a 5-character password is refused" '[[ $(wc -l < "$T/k/cmds") == 1 ]]'
curl -s -o /dev/null -d 'n=43616665&p=' $C/join
curl -s -o /dev/null -d 'name=My+Net%21&p=password123' $C/join
check "open network, typed name" '[[ "$(sed -n 2p "$T/k/cmds")" == "join 43616665 -" && "$(sed -n 3p "$T/k/cmds")" == "join 4d79204e657421 "* ]]'
printf 'connected 6950686f6e65 172.20.10.6/28' > "$T/k/net.op2wlan_state"
page=$(curl -s $C/)
check "connected: name, address, Disconnect" '[[ "$page" == *"&#10003;</span> iPhone</span><span class=\"meta\">172.20.10.6/28"* && "$page" == *Disconnect* ]]'
curl -s -o /dev/null -d 'n=6950686f6e65' $C/forget
check "forget the connected one: leave" '[[ "$(tail -1 "$T/k/cmds")" == leave && "$(grep -c 6950686f6e65 "$T/state/op2netd/networks")" == 0 ]]'
printf 'failed 43616665 STEP 5 FAIL: x <script>' > "$T/k/net.op2wlan_state"
check "failure text escaped" '[[ "$(curl -s $C/)" == *"STEP 5 FAIL: x &lt;script&gt;"* ]]'
curl -s -o /dev/null -X POST $C/scan; curl -s -o /dev/null -X POST $C/off
check "scan, off" '[[ "$(tail -2 "$T/k/cmds" | tr "\n" " ")" == "scan off " ]]'
kill $PID; wait $PID 2>/dev/null || true
: > "$T/k/cmds"; printf 'idle' > "$T/k/net.op2wlan_state"
"$T/netd" > /dev/null 2>&1 &
PID=$!
sleep 4.5
check "idle at start: joins the saved network it hears (Cafe), once" '[[ "$(cat "$T/k/cmds")" == "join 43616665 -" ]]'
exit $fail
