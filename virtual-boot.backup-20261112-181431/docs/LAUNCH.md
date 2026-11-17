# Virtual Boot ARM64 - Launch Documentation

## Overview
This package provides a standalone ARM64 Virtual Boot environment for Samsung SM-A205W (Exynos 7885).
It is designed to be loaded via kexec from TWRP recovery and transfer control to EDK2/UEFI.

## Components
- `Image` - Linux kernel (Virtual Boot minimal config)
- `exynos7885-a205w.dtb` - Device Tree Blob for SM-A205W
- `initramfs.cpio.gz` - Minimal userspace with BusyBox, diagnostics, and kexec-uefi helper
- `hardware-manifest.json` - Hardware specification and memory map
- `SHA256SUMS` - Integrity verification

## Memory Layout (validated against DeviceMemoryMap.h)
```
Kernel load:      0x80080000
Initramfs load:   0x84000000
DTB load:         0x88000000
UEFI FD (target): 0x90000000
Trampoline:       0x88000000 (same as DTB area)
```

### Validated Memory Regions
| Region | Address Range | Size | Type |
|--------|--------------|------|------|
| RAM Bank 0 Low | 0x80000000 - 0x8FFFFFFF | 256 MiB | System RAM |
| UEFI FD (BootShim) | 0x90000000 - 0x901FFFFF | 2 MiB | Reserved |
| RAM Bank 0 Mid | 0x90200000 - 0xB7FFFFFF | 638 MiB | System RAM |
| TIMA Reserved | 0xB8000000 - 0xB81FFFFF | 2 MiB | Reserved |
| RAM Bank 0 High | 0xB8200000 - 0xBBBFFFFF | 58 MiB | System RAM |
| seclog Reserved | 0xC0000000 - 0xC007FFFF | 512 KiB | Reserved |
| RAM Bank 1 Low | 0xC0080000 - 0xE8FFFFFF | 655.5 MiB | System RAM |
| WiFi/BT Reserved | 0xE9000000 - 0xE93FFFFF | 4 MiB | Reserved |
| ABOX Reserved | 0xE9400000 - 0xE97FFFFF | 4 MiB | Reserved |
| RAM Bank 1 Mid | 0xE9800000 - 0xEBFFFFFF | 40 MiB | System RAM |
| Framebuffer | 0xEC000000 - 0xED3FFFFF | 20 MiB | Reserved |
| RAM Bank 1 High | 0xED400000 - 0xF0BFFFFF | 56 MiB | System RAM |
| CMA video_stream | 0xF0C00000 - 0xF53FFFFF | 72 MiB | Reserved (CMA) |
| Modem/CP Reserved | 0xF5400000 - 0xFEBFFFFF | 152 MiB | Reserved |
| RAM Bank 1 VeryHigh | 0xFEC00000 - 0xFFBFFFFF | 16 MiB | System RAM |
| GNSS Reserved | 0xFFC00000 - 0xFFFFFFFF | 4 MiB | Reserved |
| CMA Pool cma-3 | 0x8BA000000 - 0x8BB3FFFFF | 20 MiB | Reserved (CMA) |
| CMA Pool crypto | 0x8BB800000 - 0x8BD7FFFFF | 32 MiB | Reserved (CMA) |
| CMA Pool video_frame | 0x8BD800000 - 0x8BFFFFFFF | 40 MiB | Reserved (CMA) |
| RAM Bank 2 Low | 0x880000000 - 0x8B9FFFFFF | 928 MiB | System RAM |
| RAM Bank 2 Gap1 | 0x8BB400000 - 0x8BB7FFFFF | 4 MiB | System RAM |

✅ **All regions validated - no overlaps detected**

## Loading from TWRP (kexec)
```bash
# In TWRP recovery shell:
kexec -l /sdcard/virtual-boot/Image \
  --initrd=/sdcard/virtual-boot/initramfs.cpio.gz \
  --dtb=/sdcard/virtual-boot/exynos7885-a205w.dtb \
  --command-line="console=ttySAC2,115200 earlycon=exynos4210_serial,0x12C00000"
kexec -e
```

## Boot Flow
1. TWRP loads Virtual Boot kernel + initramfs + DTB via kexec
2. Virtual Boot kernel boots, mounts initramfs
3. init script runs diagnostics, shows hardware info
4. User can run `hwinfo`, `verify-bundle`, inspect `/proc/*`
5. Run `kexec-uefi` to transfer to EDK2 at 0x90000000
6. Trampoline handles: GIC mask, CPU down, cache clean, MMU off, branch to UEFI

## Verification
```bash
# Verify all artifacts
sha256sum -c SHA256SUMS

# Verify bundle on device
verify-bundle /sdcard/uefi/UEFI /sdcard/uefi/UEFI.manifest.json
```

## Available Commands in initramfs
- `hwinfo` - Display hardware information (CPU, memory, DTB, kernel cmdline)
- `verify-bundle` - Verify UEFI bundle integrity against manifest
- `kexec-uefi` - Execute Linux → UEFI handoff (Phase 2)
- Standard BusyBox utilities: `ls`, `cat`, `dmesg`, `mount`, `dd`, `hexdump`, etc.

## Phase 2 (Future - Not Implemented)
- Trampoline binary integration (ARM64 assembly)
- Automated UEFI handoff without interactive shell
- TWRP button integration (modify existing "Start EDK2" button action)

## Security Notes
- No partitions (boot, recovery, vbmeta) are modified
- No user data is touched
- TWRP kernel/ramdisk/interface unchanged
- All builds via GitHub Actions only
