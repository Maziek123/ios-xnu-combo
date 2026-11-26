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
	'[[ "$page" == *"<a class=\"pick\" href=\"/join?n=6950686f6e65\"><span class=\"name\">iPhone</span>"* && "$page" == *">Cafe"* && "$page" == *"&lt;b&gt;&lt;/b&gt;"* ]]'
check "open network: a join button, no password page" '[[ "$page" == *"value=\"43616665\"><button class=\"pick\">"* ]]'
check "password page" '[[ "$(curl -s "$C/join?n=6950686f6e65")" == *"<h1>iPhone</h1>"*"type=\"password\""* ]]'
code=$(curl -s -o /dev/null -w '%{http_code}' -d 'n=6950686f6e65&p=simulated-ap' $C/join)
pmk=$(python3 -c "import hashlib;print(hashlib.pbkdf2_hmac('sha1',b'simulated-ap',b'iPhone',4096,32).hex())")
check "join: 303, PMK = PBKDF2(password, name)" '[[ $code == 303 && "$(tail -1 "$T/k/cmds")" == "join 6950686f6e65 $pmk" ]]'
check "saved 0600" '[[ "$(stat -c %a "$T/state/op2netd/networks")" == 600 && "$(grep ^6950686f6e65 "$T/state/op2netd/networks")" == "6950686f6e65 $pmk" ]]'
curl -s -o /dev/null -d 'n=6950686f6e65&p=short' $C/join
check "a 5-character password is refused" '[[ $(wc -l < "$T/k/cmds") == 1 ]]'
page=$(curl -s $C/)
check "refusal shown once on the page" '[[ "$page" == *"class=\"note\">A WPA2 password has 8 to 63"* && "$(curl -s $C/)" != *"class=\"note\""* ]]'
check "saved: tap joins (button), menu: change password, forget" '[[ "$page" == *"value=\"6950686f6e65\"><button class=\"pick\"><span class=\"name\">iPhone</span><span class=\"meta\">saved &middot;"*"<summary>"*"href=\"/join?n=6950686f6e65\">Change password"*"Forget this network"* ]]'
check "heard saved networks not listed again under Saved" '[[ "$page" != *"Saved, not in range"* ]]'
curl -s -o /dev/null -d 'n=6950686f6e65' $C/join
check "tap a saved network: its saved PMK, file unchanged" '[[ "$(tail -1 "$T/k/cmds")" == "join 6950686f6e65 $pmk" && "$(grep ^6950686f6e65 "$T/state/op2netd/networks")" == "6950686f6e65 $pmk" ]]'
curl -s -o /dev/null -d 'n=6950686f6e65&p=' $C/join
check "empty password field on a saved one: saved PMK kept" '[[ "$(tail -1 "$T/k/cmds")" == "join 6950686f6e65 $pmk" && "$(grep ^6950686f6e65 "$T/state/op2netd/networks")" == "6950686f6e65 $pmk" ]]'
check "change-password page for a saved one" '[[ "$(curl -s "$C/join?n=6950686f6e65")" == *"New password (empty: keep the saved one)"* ]]'
before=$(wc -l < "$T/k/cmds")
curl -s -o /dev/null -d 'n=3c623e3c2f623e&p=' $C/join
check "WPA2 network, not saved, empty password: refused, not saved as open" '[[ $(wc -l < "$T/k/cmds") == $before && "$(cat "$T/state/op2netd/networks")" != *3c623e3c2f623e* && "$(curl -s $C/)" == *"needs a password: not joined"* ]]'
curl -s -o /dev/null -d 'n=43616665&p=' $C/join
curl -s -o /dev/null -d 'name=My+Net%21&p=password123' $C/join
check "open network, typed name" '[[ "$(tail -2 "$T/k/cmds" | head -1)" == "join 43616665 -" && "$(tail -1 "$T/k/cmds")" == "join 4d79204e657421 "* ]]'
printf 'connected 6950686f6e65 172.20.10.6/28' > "$T/k/net.op2wlan_state"
printf 'wl0 172.20.10.6 255.255.255.240 172.20.10.1 172.20.10.1' > "$T/k/net.op2wlan"
printf '02:00:00:00:5c:01 8388608 1610612736' > "$T/k/test.wl0"
page=$(curl -s $C/)
check "connected: name, address, Disconnect" '[[ "$page" == *"&#10003;</span> iPhone</span><span class=\"meta\" style=\"margin:0\">172.20.10.6/28"* && "$page" == *Disconnect* ]]'
check "connected: details (address, mask, router, DNS, channel, MAC, bytes, time)" '[[ "$page" == *"<details class=\"info\" id=\"info\">"*"IP address</span><span>172.20.10.6"*"Subnet mask</span><span>255.255.255.240"*"Router</span><span>172.20.10.1"*"DNS 1</span><span>172.20.10.1</span>"*"DNS 2</span><span>1.1.1.1</span>"*"Channel</span><span>6 (2.4 GHz)"*"Access point</span><span>82:98:3e:cf:7c:84"*"Wi-Fi address</span><span>02:00:00:00:5c:01"*"Received</span><span>8.0 MB"*"Sent</span><span>1.50 GB"*"Connected for"* ]]'
# Advanced: per-network settings
adv=$(curl -s "$C/net?n=6950686f6e65")
check "advanced page: switches, IPv4 choice, DNS fields, the network's values as hints" '[[ "$adv" == *"<h1>iPhone</h1>"*"name=\"auto\" checked"*"<option value=\"dhcp\" selected>"*"placeholder=\"172.20.10.6\""*"name=\"d1\" value=\"\" placeholder=\"172.20.10.1\""*"name=\"dnsauto\" checked"* ]]'
check "advanced page of a network not saved: join it first" '[[ "$(curl -s "$C/net?n=43616666")" == *"Join this network from the list first"* ]]'
check "menu and details link to Advanced" '[[ "$(curl -s $C/)" == *"href=\"/net?n=6950686f6e65\" style="*">Advanced&hellip;</a>"*"href=\"/net?n=6950686f6e65\">Advanced&hellip;</a><a href=\"/join?n=6950686f6e65\">Change password"* ]]'
before=$(wc -l < "$T/k/cmds")
code=$(curl -s -o /dev/null -w '%{http_code}' --data-urlencode 'n=6950686f6e65' -d 'ipm=dhcp&d1=9.9.9.9&d2=149.112.112.112&d3=' --data-urlencode 'search=lan, example.com' $C/net)
check "DNS + search saved (switches off: no auto-join, only these servers), no rejoin for a DNS change" '[[ $code == 303 && "$(grep ^6950686f6e65 "$T/state/op2netd/networks")" == "6950686f6e65 $pmk auto=0 dns=9.9.9.9,149.112.112.112 dnsauto=0 search=lan,example.com" && $(wc -l < "$T/k/cmds") == $before ]]'
touch "$T/k/wl0_up"; sleep 3.5
check "configd gets those DNS servers (in order) and search domains" '[[ "$(tail -1 "$T/k/published")" == *"172.20.10.6 255.255.255.240 172.20.10.1 9.9.9.9 149.112.112.112 lan example.com "* ]]'
page=$(curl -s $C/)
check "details: one row per DNS server, search domains" '[[ "$page" == *"DNS 1</span><span>9.9.9.9</span></div><div class=\"kv\"><span>DNS 2</span><span>149.112.112.112</span>"*"Search domains</span><span>lan,example.com</span>"*"IPv4</span><span>Automatic (DHCP)"* ]]'
curl -s -o /dev/null -d 'n=6950686f6e65&auto=on&dnsauto=on&ipm=dhcp&d1=9.9.9.9' $C/net; sleep 3.5
check "also the network's DNS: appended after its own" '[[ "$(tail -1 "$T/k/published")" == *" 172.20.10.1 9.9.9.9 172.20.10.1 "* && "$(grep ^6950686f6e65 "$T/state/op2netd/networks")" == "6950686f6e65 $pmk dns=9.9.9.9" ]]'
curl -s -o /dev/null -d 'n=6950686f6e65&auto=on&dnsauto=on&ipm=dhcp' $C/net; sleep 3.5
check "no servers: the default again (the network's, then 1.1.1.1)" '[[ "$(tail -1 "$T/k/published")" == *" 172.20.10.1 172.20.10.1 1.1.1.1 "* ]]'
bad=$(curl -s -d 'n=6950686f6e65&auto=on&ipm=manual&ip=172.20.10.9&mask=255.255.255.240&router=10.0.0.1' $C/net)
check "manual: a router outside the subnet is refused, the form comes back as typed" '[[ "$bad" == *"Router: it must be another address in the same subnet"*"<option value=\"manual\" selected>"*"name=\"ip\" value=\"172.20.10.9\""* && "$(grep ^6950686f6e65 "$T/state/op2netd/networks")" == "6950686f6e65 $pmk" ]]'
check "manual: a broken mask, a bad DNS server refused" '[[ "$(curl -s -d "n=6950686f6e65&ipm=manual&ip=172.20.10.9&mask=255.0.255.0&router=172.20.10.1" $C/net)" == *"Subnet mask: not a mask"* && "$(curl -s -d "n=6950686f6e65&ipm=dhcp&d2=1.2.3" $C/net)" == *"DNS server 2: not an IPv4"* ]]'
curl -s -o /dev/null -d 'n=6950686f6e65&auto=on&dnsauto=on&ipm=manual&ip=172.20.10.9&mask=255.255.255.240&router=172.20.10.1&d1=9.9.9.9' $C/net
check "manual IPv4 while connected: saved, join again with it (static, the first DNS server)" '[[ "$(tail -1 "$T/k/cmds")" == "join 6950686f6e65 $pmk static ac140a09 fffffff0 ac140a01 09090909" && "$(grep ^6950686f6e65 "$T/state/op2netd/networks")" == "6950686f6e65 $pmk ip=172.20.10.9 mask=255.255.255.240 router=172.20.10.1 dns=9.9.9.9" ]]'
check "the note says so" '[[ "$(curl -s $C/)" == *"iPhone: saved; reconnecting with the new IPv4 setting"* ]]'
curl -s -o /dev/null -d 'n=6950686f6e65&p=another-password' $C/join
pmk2=$(python3 -c "import hashlib;print(hashlib.pbkdf2_hmac('sha1',b'another-password',b'iPhone',4096,32).hex())")
check "a new password keeps the settings; the join carries the manual address" '[[ "$(grep ^6950686f6e65 "$T/state/op2netd/networks")" == "6950686f6e65 $pmk2 ip=172.20.10.9 mask=255.255.255.240 router=172.20.10.1 dns=9.9.9.9" && "$(tail -1 "$T/k/cmds")" == "join 6950686f6e65 $pmk2 static "* ]]'
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
kill $PID; wait $PID 2>/dev/null || true
sed -i 's/^43616665 -$/43616665 - auto=0/' "$T/state/op2netd/networks"; : > "$T/k/cmds"
"$T/netd" > /dev/null 2>&1 &
PID=$!
sleep 4.5
check "auto-join off: not joined automatically" '[[ ! -s "$T/k/cmds" ]]'
exit $fail
