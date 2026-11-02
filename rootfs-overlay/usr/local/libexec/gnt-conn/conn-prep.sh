#!/bin/sh
# gnt-conn-prep - consys power-on for MT6833 (GNT-AN00).
#
# Order matters and is not negotiable:
#   btif      exports mtk_wcn_btif_* symbols that wmt_drv needs
#   wmt_drv   pmic_hw_lp=0: the MT6359 connectivity LDO hardware voters
#             must stay inert. With a working pmic phandle they succeed and
#             put VCN33_2 (Wi-Fi PA) into low power, after which cfg80211
#             scans fail with WLAN_STATUS_ADAPTER_NOT_READY (0xC0010011).
#   wmtload hwinit
#             the consys platform driver is registered and bound BY THIS IOCTL,
#             not at module load. Waiting for the bind before running wmtload
#             deadlocks (learned on kernel #86/#87).
#
# The modules live in the kernel module tree (extra/) and are indexed by
# depmod, so modprobe resolves them - no hardcoded /opt paths.
D=/usr/local/libexec/gnt-conn

#
# GNT_CONN_MUTEX
#
# wmtload hwinit re-binds the consys platform driver. Running it twice at the
# same time makes the second instance poke a device the first one is still
# binding, which shows up as "Driver 'mtk_sdio_client' is already registered,
# aborting..." and can wedge the bus hard enough that a later sync() never
# returns - the machine then hangs with the desktop half-drawn and no network.
#
# Observed on hardware: three different PIDs all running wmtload in the same
# second during the boot that hung, versus one PID at a time (11 s apart) on
# boots that succeeded. The first boot of a session is where it happens,
# because that is when several units race to start at once.
#
# Take an exclusive lock for the whole sequence, and make the body idempotent:
# if the chip is already initialised there is nothing to do.
#
exec 9>/run/gnt-conn-prep.lock
if ! flock -w 100 9; then
    echo "another instance still holds the lock after 100 s" >&2
    exit 1
fi

echo "=== 1. modules (btif before wmt_drv) ==="
[ -d /sys/module/btif ]    || modprobe btif    || exit 1
[ -d /sys/module/wmt_drv ] || modprobe wmt_drv pmic_hw_lp=0 || exit 1
echo "modules loaded"

# Already bound by an earlier run? Then the chip init is already done.
if [ -L /sys/bus/platform/devices/18002000.consys/driver ]; then
    echo "consys already bound to $(basename "$(readlink -f /sys/bus/platform/devices/18002000.consys/driver)") - nothing to do"
    exit 0
fi

echo "=== 2. chip init (registers + binds the consys platform driver) ==="
sleep 1
$D/tools/wmtload hwinit soc set adie init
RC=$?
echo "wmtload rc=$RC"

echo "=== 3. verify the consys bind ==="
sleep 2
ls -ld /sys/bus/platform/devices/18002000.consys/driver 2>&1

#
# No sync here. The original script called sync to flush whatever wmtload had
# written, but that is what turned a consys problem into a dead machine: sync
# waits for writeback on the root filesystem, and when the storage path is
# wedged the whole system stops with no console and no network. There is
# nothing in this script that needs a full-system sync - wmtload's own writes
# are already committed by the driver.
#

[ $RC -eq 0 ]
