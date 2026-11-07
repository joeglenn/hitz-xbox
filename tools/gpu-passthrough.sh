#!/bin/bash
# gpu-passthrough.sh — hand odin's Radeon 8060S (bc:00.0) to the win11 VM, or give it back.
#   sudo ~/hitz-xbox/tools/gpu-passthrough.sh on     # stop GPU services, dump VBIOS, bind vfio-pci
#   sudo ~/hitz-xbox/tools/gpu-passthrough.sh off    # rebind amdgpu, restart llama-router + comfyui
#   sudo ~/hitz-xbox/tools/gpu-passthrough.sh status
# While "on", the host has NO GPU: llama-router and ComfyUI are stopped.
# If "off" fails to bring the GPU back (AMD iGPUs have no FLR), reboot odin; that always restores it.
#
# Machine-specific developer tool for the "odin" dev box: GPU=the Radeon
# 8060S function, AUD=its HDMI audio function on the same card, both PCI
# devices identified by their bus:device.function address. "on" moves both
# from their normal driver (amdgpu / snd_hda_intel) to vfio-pci so a VM can
# own the hardware directly; "off" reverses that. All actions run as root
# (sudo) because they poke /sys/bus/pci driver-binding files. Every run
# appends a timestamped log line to $OUT/passthrough.log via the `tee`
# redirect near the top, in addition to whatever is printed to the terminal.
set -u
GPU=0000:bc:00.0
AUD=0000:bc:00.1
OWNER=jbglenn
OUT=/home/jbglenn/hitz-xbox/vm
LOG=$OUT/passthrough.log
mkdir -p "$OUT"
exec > >(tee -a "$LOG") 2>&1
echo "=== $(date -Is) $0 ${1:-}"

[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }

# drv ADDR: kernel driver currently bound to PCI device ADDR, or "none".
drv() { basename "$(readlink -f /sys/bus/pci/devices/$1/driver 2>/dev/null)" 2>/dev/null || echo none; }
# grp ADDR: the IOMMU group number ADDR belongs to (used to find /dev/vfio/<group>).
grp() { basename "$(readlink -f /sys/bus/pci/devices/$1/iommu_group)"; }

# Pull the GPU's video BIOS image out of the firmware's ACPI VFCT table and
# save it to $OUT/vbios_1586.rom, so it can later be handed to the guest
# (see tools/win11-gpu.sh and tools/mk_vfct.py). Needed because the Strix
# Halo APU has no discrete VBIOS ROM to read directly; the ACPI VFCT table
# (set up by firmware at boot) is the only place to get it from on the host.
dump_vbios() {
    python3 - "$OUT/vbios_1586.rom" <<'EOF'
import struct, sys
t = open("/sys/firmware/acpi/tables/VFCT", "rb").read()
off = struct.unpack_from("<I", t, 52)[0]
found = False
while off + 28 <= len(t):
    bus, dev, fn, ven, did, ssv, ssid, rev, ln = struct.unpack_from("<IIIHHHHII", t, off)
    img = t[off + 28: off + 28 + ln]
    print("VFCT image: %02x:%02x.%x %04x:%04x len %d sig %s" % (bus, dev, fn, ven, did, ln, img[:2].hex()))
    if ven == 0x1002 and img[:2] == b"\x55\xaa":
        open(sys.argv[1], "wb").write(img)
        print("wrote", sys.argv[1]); found = True; break
    if ln == 0: break
    off += 28 + ln
sys.exit(0 if found else 1)
EOF
}

case "${1:-status}" in
on)
    echo "stopping GPU users"
    systemctl stop llama-router.service comfyui.service || true
    sleep 2
    if command -v fuser >/dev/null && fuser -v /dev/dri/* 2>&1 | grep -v "^ *USER" | grep -q .; then
        echo "still open on /dev/dri:"; fuser -v /dev/dri/* ; echo "stop those processes and re-run"; exit 1
    fi
    dump_vbios || { echo "VBIOS dump failed"; exit 1; }
    chown $OWNER: "$OUT/vbios_1586.rom"
    modprobe vfio-pci
    # Detach any virtual console and the EFI framebuffer so the kernel stops
    # touching the display before the GPU's PCI driver is unbound: otherwise
    # the unbind can hang or the console can corrupt the screen.
    for vc in /sys/class/vtconsole/vtcon*/bind; do echo 0 > "$vc" 2>/dev/null || true; done
    echo efi-framebuffer.0 > /sys/bus/platform/drivers/efi-framebuffer/unbind 2>/dev/null || true
    for d in $AUD $GPU; do
        echo "$d: $(drv $d) -> vfio-pci"
        [ "$(drv $d)" != none ] && echo $d > /sys/bus/pci/devices/$d/driver/unbind
        echo vfio-pci > /sys/bus/pci/devices/$d/driver_override
        echo $d > /sys/bus/pci/drivers/vfio-pci/bind
    done
    for d in $GPU $AUD; do
        g=$(grp $d); chown $OWNER: /dev/vfio/$g; echo "$d driver=$(drv $d) group=$g /dev/vfio/$g"
    done
    echo "done. Now (no sudo needed): ~/hitz-xbox/tools/win11-gpu.sh gpu"
    ;;
off)
    for d in $GPU $AUD; do
        echo "$d: $(drv $d) -> host"
        [ "$(drv $d)" != none ] && echo $d > /sys/bus/pci/devices/$d/driver/unbind
        echo > /sys/bus/pci/devices/$d/driver_override
    done
    echo $GPU > /sys/bus/pci/drivers_probe
    echo $AUD > /sys/bus/pci/drivers_probe
    sleep 3
    echo "GPU driver now: $(drv $GPU)"; ls /dev/dri
    for vc in /sys/class/vtconsole/vtcon*/bind; do echo 1 > "$vc" 2>/dev/null || true; done
    if [ "$(drv $GPU)" = amdgpu ] && [ -e /dev/dri/renderD128 ]; then
        systemctl start llama-router.service comfyui.service
        echo "services restarted"
    else
        echo "amdgpu did not come back cleanly: reboot odin to restore the GPU"
    fi
    ;;
status)
    for d in $GPU $AUD; do echo "$d driver=$(drv $d) group=$(grp $d)"; done
    systemctl is-active llama-router.service comfyui.service
    ls -la /dev/vfio
    ;;
*) echo "usage: $0 on|off|status"; exit 1 ;;
esac
