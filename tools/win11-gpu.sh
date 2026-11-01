#!/bin/bash
# win11-gpu.sh — recreate the win11 container with or without the passed-through GPU.
#   ~/hitz-xbox/tools/win11-gpu.sh gpu     # after: sudo gpu-passthrough.sh on
#   ~/hitz-xbox/tools/win11-gpu.sh plain   # before: sudo gpu-passthrough.sh off
# Same disk (/home/jbglenn/winvm/storage), ports and credentials as the original container.
set -eu
GPU=0000:bc:00.0
AUD=0000:bc:00.1
VM=/home/jbglenn/hitz-xbox/vm
mode=${1:-}
pw=$(docker inspect win11 --format '{{range .Config.Env}}{{println .}}{{end}}' | sed -n 's/^PASSWORD=//p')
[ -n "$pw" ] || { echo "could not read PASSWORD from the current container"; exit 1; }

common=(--name win11 --restart unless-stopped --stop-timeout 120
    --device /dev/kvm --device /dev/net/tun --cap-add NET_ADMIN
    -p 127.0.0.1:2222:22 -p 127.0.0.1:3389:3389 -p 192.168.1.83:8006:8006
    -v /home/jbglenn/winvm/storage:/storage -v /home/jbglenn/winvm/oem:/oem
    -e VERSION=11 -e RAM_SIZE=16G -e CPU_CORES=8 -e DISK_SIZE=128G
    -e USERNAME=hitz -e "PASSWORD=$pw")

case "$mode" in
gpu)
    g1=$(basename "$(readlink -f /sys/bus/pci/devices/$GPU/iommu_group)")
    g2=$(basename "$(readlink -f /sys/bus/pci/devices/$AUD/iommu_group)")
    for d in $GPU $AUD; do
        [ "$(basename "$(readlink -f /sys/bus/pci/devices/$d/driver)")" = vfio-pci ] \
            || { echo "$d is not on vfio-pci: run sudo gpu-passthrough.sh on first"; exit 1; }
    done
    [ -f $VM/vbios_1586.rom ] || { echo "missing $VM/vbios_1586.rom"; exit 1; }
    extra=(--device /dev/vfio/vfio --device /dev/vfio/$g1 --device /dev/vfio/$g2
        --cap-add IPC_LOCK --ulimit memlock=-1:-1 -v $VM:/vbios:ro
        -e "ARGUMENTS=-device pcie-root-port,id=gpurp,chassis=8,slot=8,bus=pcie.0,addr=0x1c -device vfio-pci,host=$GPU,bus=gpurp,addr=0x0.0x0,multifunction=on,romfile=/vbios/vbios_1586.rom,x-vga=off -device vfio-pci,host=$AUD,bus=gpurp,addr=0x0.0x1,romfile=/vbios/AMDGopDriver.rom -acpitable file=/vbios/vfct_guest.bin")
    # Layout from working Strix Halo passthrough configs: GPU behind a PCIe root port, GPU+audio as
    # one multifunction device, the UEFI GOP driver (AMDGopDriver.rom) on the audio function so OVMF
    # initialises the GPU. vfct_guest.bin: tools/mk_vfct.py for the guest address (01:00.0 here) --
    # the APU driver reads its VBIOS from ACPI VFCT (code 43 without the GOP/VFCT).
    [ -f $VM/AMDGopDriver.rom ] || { echo "missing $VM/AMDGopDriver.rom"; exit 1; }
    [ -f $VM/vfct_guest.bin ] || { echo "missing $VM/vfct_guest.bin (tools/mk_vfct.py)"; exit 1; }
    ;;
plain) extra=() ;;
*) echo "usage: $0 gpu|plain"; exit 1 ;;
esac

docker stop -t 120 win11 >/dev/null || true
docker rm win11 >/dev/null
docker run -d "${common[@]}" "${extra[@]}" dockurr/windows
echo "win11 recreated ($mode). Boot takes ~1-2 min; ssh -p 2222 hitz@127.0.0.1"
