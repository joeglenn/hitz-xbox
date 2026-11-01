#!/bin/bash
# gpu-vfio-boot.sh — boot odin ONCE with the Radeon 8060S on vfio-pci (for the win11 VM).
# Runtime unbinding of amdgpu oopses on this kernel (isp_genpd_remove_device), so the GPU
# is handed to vfio-pci at boot instead, by a separate GRUB entry that is never the default.
#
#   sudo ~/hitz-xbox/tools/gpu-vfio-boot.sh setup    # once: add the GRUB entry + vfio in initramfs
#   sudo ~/hitz-xbox/tools/gpu-vfio-boot.sh once     # next boot only uses the vfio entry, then reboot
#   sudo reboot                                      # any later normal reboot = host GPU again
#
# In the vfio boot: amdgpu is blacklisted, 1002:1586 (GPU) and 1002:1640 (its HDMI audio) go to
# vfio-pci, and llama-router + comfyui are masked for that boot only (systemd.mask on the cmdline).
set -eu
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
ENTRY="odin-gpu-vfio"
ARGS="modprobe.blacklist=amdgpu vfio-pci.ids=1002:1586,1002:1640 amd_iommu=on iommu=pt systemd.mask=llama-router.service systemd.mask=comfyui.service"

case "${1:-}" in
setup)
    grep -q '^vfio_pci$' /etc/initramfs-tools/modules || printf 'vfio\nvfio_iommu_type1\nvfio_pci\n' >> /etc/initramfs-tools/modules
    echo "softdep amdgpu pre: vfio-pci" > /etc/modprobe.d/odin-vfio.conf
    echo "softdep snd_hda_intel pre: vfio-pci" >> /etc/modprobe.d/odin-vfio.conf
    update-initramfs -u
    # Clone the first menuentry of the generated grub.cfg, rename it, add ARGS to its linux line.
    python3 - "$ENTRY" "$ARGS" <<'EOF'
import re, sys
name, args = sys.argv[1], sys.argv[2]
cfg = open("/boot/grub/grub.cfg").read()
m = re.search(r"^menuentry '[^']*'.*?^}", cfg, re.S | re.M)
if not m: sys.exit("no menuentry in grub.cfg")
e = m.group(0)
e = re.sub(r"^menuentry '[^']*'", "menuentry '%s'" % name, e, count=1, flags=re.M)
e = re.sub(r"\$menuentry_id_option '[^']*'", "$menuentry_id_option '%s'" % name, e, count=1)
e = re.sub(r"^(\s*linux\s+\S+.*)$", lambda mm: mm.group(1) + " " + args, e, count=1, flags=re.M)
open("/etc/grub.d/42_odin_gpu_vfio", "w").write("#!/bin/sh\nexec tail -n +3 $0\n" + e + "\n")
print(e)
EOF
    chmod +x /etc/grub.d/42_odin_gpu_vfio
    grep -q '^GRUB_DEFAULT=saved' /etc/default/grub || {
        sed -i 's/^GRUB_DEFAULT=.*/GRUB_DEFAULT=saved/' /etc/default/grub
        grub-set-default 0
    }
    update-grub
    grep -c "$ENTRY" /boot/grub/grub.cfg
    echo "setup done. Next: sudo $0 once && sudo reboot"
    ;;
once)
    grub-reboot "$ENTRY"
    grub-editenv list
    echo "next boot -> $ENTRY (once). Now: sudo reboot"
    ;;
*) echo "usage: $0 setup|once"; exit 1 ;;
esac
