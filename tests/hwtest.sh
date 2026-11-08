#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Load a freshly built minisforum_ec.ko on a supported board, check it, then put
# back the module that was loaded before. Run it through "make hwtest".
set -u
KO=${1:?usage: hwtest.sh minisforum_ec.ko}
P=/sys/module/minisforum_ec/parameters
CPU_DEFAULT="0:0,25:1000,45:1400,54:1800,66:firmware"
fail=0

if [ "$(id -u)" != 0 ]; then
	echo "hwtest.sh: needs root; run it through make hwtest" >&2
	exit 1
fi

check() { # description, command...
	local what=$1
	shift
	if "$@" >/dev/null 2>&1; then echo "PASS $what"; else
		echo "FAIL $what"
		fail=1
	fi
}
msgs() { dmesg | tail -n +"$((DMESG_START + 1))"; }
owned() { msgs | grep -q "$1 fan under driver control"; }

restore() {
	rmmod minisforum_ec 2>/dev/null
	for fan in cpu system; do
		if owned $fan; then
			check "$fan fan handed back on unload" grep -q "$fan fan returned to firmware: driver unload" <(msgs)
		fi
	done
	if [ "$WAS_LOADED" = 1 ]; then
		modprobe minisforum_ec && echo "restored the installed module $(modinfo -F version minisforum_ec)"
	fi
	echo "== driver messages during the test"
	msgs | grep minisforum_ec
	rm -rf "$TMP"
	if [ "$fail" = 0 ]; then echo "RESULT: all checks passed"; else echo "RESULT: some checks FAILED"; fi
	exit "$fail"
}

# Secure Boot: sign a copy with the key DKMS signs with (Ubuntu's, or DKMS's own).
TMP=$(mktemp -d)
cp "$KO" "$TMP/minisforum_ec.ko"
KO=$TMP/minisforum_ec.ko
SIGN=/lib/modules/$(uname -r)/build/scripts/sign-file
if [ -r /var/lib/shim-signed/mok/MOK.priv ]; then
	"$SIGN" sha512 /var/lib/shim-signed/mok/MOK.priv /var/lib/shim-signed/mok/MOK.der "$KO"
elif [ -r /var/lib/dkms/mok.key ]; then
	"$SIGN" sha512 /var/lib/dkms/mok.key /var/lib/dkms/mok.pub "$KO"
fi

WAS_LOADED=0
[ -d /sys/module/minisforum_ec ] && WAS_LOADED=1
DMESG_START=$(dmesg | wc -l)
trap restore EXIT
if [ "$WAS_LOADED" = 1 ] && ! rmmod minisforum_ec; then
	echo "FAIL unloading the installed module"
	fail=1
	exit
fi
if ! insmod "$KO"; then
	echo "FAIL loading $1"
	fail=1
	exit
fi
echo "loaded the test module $(cat /sys/module/minisforum_ec/version)"
sleep 5
H=$(dirname "$(grep -l '^minisforum_ec$' /sys/class/hwmon/*/name)")
T1=$(($(cat "$H/temp1_input") / 1000))
T2=$(($(cat "$H/temp2_input") / 1000))

check "supported board detected" grep -qE 'F7BS[IC] \(UM' <(msgs)
# The default curves hand a fan to the firmware from 66 C (CPU) and 50 C (system).
if [ "$T1" -lt 60 ]; then check "CPU fan under driver control" owned cpu; else echo "skip CPU fan control check: EC CPU at $T1 C"; fi
if [ "$T2" -lt 45 ]; then check "system fan under driver control" owned system; else echo "skip system fan control check: EC system at $T2 C"; fi
check "pwm1 in curve mode" test "$(cat "$H/pwm1_enable")" = 2
check "pwm2 in curve mode" test "$(cat "$H/pwm2_enable")" = 2
check "fan speeds read" grep -qE '^[0-9]+$' "$H/fan1_input" "$H/fan2_input"
check "temperatures plausible ($T1 C, $T2 C)" test "$T1" -ge 10 -a "$T1" -le 100 -a "$T2" -ge 10 -a "$T2" -le 100
check "fan labels" test "$(cat "$H/fan1_label"),$(cat "$H/fan2_label")" = "CPU,System"
check "cpu_curve default" test "$(cat "$P/cpu_curve")" = "$CPU_DEFAULT"
check "cpu_curve write accepted" sh -c "echo '$CPU_DEFAULT' > $P/cpu_curve"
check "bad curve refused" sh -c "! echo bad > $P/cpu_curve"
if [ -d /sys/kernel/debug/minisforum_ec ]; then
	check "debugfs state readable" test -n "$(cat /sys/kernel/debug/minisforum_ec/state)"
fi

echo "== CPU fan fixed at 2000 RPM for 30 s (renewed every 10 s), then back to the curve"
echo 1 >"$H/pwm1_enable"
for _ in 1 2 3; do
	echo 100 >"$H/pwm1"
	sleep 10
	echo "fan1=$(cat "$H/fan1_input")"
done
check "fixed value reached (fan1 above 1700 RPM)" test "$(cat "$H/fan1_input")" -gt 1700
echo 2 >"$H/pwm1_enable"
check "pwm1 back in curve mode" test "$(cat "$H/pwm1_enable")" = 2
sleep 10
check "no hunting, failure or refusal logged" sh -c "! dmesg | tail -n +$((DMESG_START + 1)) | grep -iE 'hunting|fail|error|unexpected|refus'"
