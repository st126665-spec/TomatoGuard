#!/usr/bin/env bash
# =====================================================================
# TomatoGuard - 4_cloud_measure.sh  (run on the Proxmox container CT 111, ~8 min)
# Facts + sysbench (CPU/mem) + dd (disk) + live usage (NO docker here)
# Saves to results_cloud_<timestamp>.txt
# =====================================================================
set -u
OUT="results_cloud_$(date +%Y%m%d_%H%M%S).txt"
exec > >(tee "$OUT") 2>&1

echo "==================================================================="
echo " TomatoGuard - Cloud container measurements    $(date)"
echo " host=$(hostname)"
echo "==================================================================="

echo; echo "### FACTS  (for the CT 111 Summary table)"
echo "CPU cores (nproc): $(nproc)"
echo "-- memory --"; free -h
echo "-- disk --";   df -h /
echo "-- OS --";     grep PRETTY_NAME /etc/os-release

command -v sysbench >/dev/null || { echo "installing sysbench..."; apt-get install -y sysbench >/dev/null 2>&1; }

echo; echo "### 2) sysbench CPU  (3 runs single-thread, 3 runs all-thread)"
for r in 1 2 3; do echo "-- CPU 1-thread run $r --"; sysbench cpu --threads=1 --time=10 run | grep "events per second"; done
for r in 1 2 3; do echo "-- CPU ${r} all-thread ($(nproc)) run $r --"; sysbench cpu --threads="$(nproc)" --time=10 run | grep "events per second"; done

echo; echo "### 3) sysbench MEMORY  (3 runs)"
for r in 1 2 3; do echo "-- memory run $r --"; sysbench memory --memory-block-size=1K --memory-total-size=10G run | grep "transferred"; done

echo; echo "### 4) dd DISK  (3 runs write + read)"
for r in 1 2 3; do
  echo "-- write run $r --"; dd if=/dev/zero of=./ddtest.tmp bs=1M count=512 conv=fdatasync 2>&1 | tail -1
  sync; echo 3 | tee /proc/sys/vm/drop_caches >/dev/null 2>&1   # may be blocked in a container; that's fine
  echo "-- read  run $r --"; dd if=./ddtest.tmp of=/dev/null bs=1M 2>&1 | tail -1
done
rm -f ./ddtest.tmp

echo; echo "### 5) iperf3 (optional - only if a server is reachable)"
if [ "${IPERF_SERVER:-}" != "" ]; then
  command -v iperf3 >/dev/null || apt-get install -y iperf3 >/dev/null 2>&1
  echo "-- TCP download --"; iperf3 -c "$IPERF_SERVER" -t 10 | tail -4
  echo "-- TCP upload   --"; iperf3 -c "$IPERF_SERVER" -t 10 -R | tail -4
  echo "-- UDP          --"; iperf3 -c "$IPERF_SERVER" -u -b 20M -t 10 | tail -4
else
  echo "  IPERF_SERVER not set -> skipped (report will mark cloud network 'not measured')."
fi

echo; echo "### 6) LIVE RESOURCE USAGE (no docker here - native services)"
systemd-cgtop -n 1 2>/dev/null | head -20 || top -bn1 | head -15
echo "-- service memory/cpu --"
systemctl status tomatoguard influxdb --no-pager 2>/dev/null | grep -E "Memory|CPU|Tasks"

echo; echo "==================================================================="
echo " DONE -> $OUT     (send this file + the CT 111 Summary screenshot to Liza)"
echo "==================================================================="
