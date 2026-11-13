# Virtual Boot ARM64 - Launch Documentation (Phase 1)

## Overview
This package provides a standalone ARM64 Virtual Boot environment for Samsung SM-A205W (Galaxy A20, Exynos 7885).
It is designed to be loaded via kexec from TWRP recovery.

**Phase 1 Status**: Build and validation of independent Virtual Boot components only.
**Phase 2 (Future)**: Linux → UEFI handoff and TWRP button integration.

## Hardware Identification
- **Model**: Samsung Galaxy A20
- **Reference**: SM-A205W
- **Platform**: Exynos 7885
- **Codename**: a20

## Components (Phase 1)
- `Image` - Linux kernel (Virtual Boot minimal config, KEXEC enabled)
- `exynos7885-a205w-virtual.dtb` - Device Tree Blob built from authentic Samsung DTS chain
- `initramfs.cpio.gz` - Minimal userspace with BusyBox for diagnostics
- `hardware-manifest.json` - Hardware specification and memory map
- `SHA256SUMS` - Integrity verification

## Memory Layout (Reference - to be validated against compiled DTB)
```
Kernel load:      0x80080000
Initramfs load:   0x84000000
DTB load:         0x88000000
UEFI FD (target): 0x90000000-0x90200000 (2 MiB reserved)
Trampoline:       NOT ASSIGNED (Phase 2)
```

### Reference Memory Regions (from DeviceMemoryMap.h)
| Region | Address Range | Size | Type |
|--------|--------------|------|------|
| RAM Bank 0 | 0x80000000 - 0xBBC00000 | 956 MiB | System RAM |
| RAM Bank 1 | 0xC0000000 - 0x100000000 | 1024 MiB | System RAM |
| RAM Bank 2 | 0x880000000 - 0x8C0000000 | 1024 MiB | System RAM (>4GB) |
| UEFI FD | 0x90000000 - 0x90200000 | 2 MiB | Reserved |

### Reserved Regions (from Samsung DTB)
- TIMA: 0xB8000000-0xB8200000
- seclog: 0xC0000000-0xC0080000
- WiFi/BT: 0xE9000000-0xE9400000
- ABOX: 0xE9400000-0xE9800000
- Framebuffer: 0xEC000000-0xED400000
- CMA video_stream: 0xF0C00000-0xF5400000
- Modem/CP: 0xF5400000-0xFEC00000
- GNSS: 0xFFC00000-0x100000000
- CMA crypto: 0x8BB800000-0x8BD800000
- CMA video_frame: 0x8BD800000-0x8C0000000

## Loading from TWRP (kexec) - NOT ENABLED

**Do not execute a kexec load or execute command on the SM-A205W yet.**
The example below was removed because the Phase 1 bundle is diagnostic-only and
the end-to-end runtime path has not been validated.

Outstanding blockers for a safe launch:

- The exact physical placement of the combined BootShim + FD image is unresolved.
- The target kernel's ordinary kexec path clears x0-x3 before branching; the
  incoming FDT pointer therefore cannot be assumed to reach BootShim.
- The EDK2 SEC entry address and its required execution state are known from
  static artifact inspection, but have not been executed on hardware.
- The target kernel requires secondary CPUs to be quiesced for ordinary kexec.
- DMA-capable devices must be quiesced before ownership of memory is transferred.

The TWRP “Démarrer EDK2” action must remain disabled until these blockers have
a reviewed implementation and validation evidence. CI packaging success is
not runtime boot evidence.

## Available Commands in initramfs
- `hwinfo` - Display hardware information (CPU, memory, DTB, kernel cmdline)
- Standard BusyBox utilities: `ls`, `cat`, `dmesg`, `mount`, `dd`, `hexdump`, etc.

## Verification
```bash
# Verify all artifacts
sha256sum -c SHA256SUMS
```

## Phase 2 (Future - Not Implemented)
- [ ] Trampoline binary (ARM64 assembly) compilation and integration
- [ ] Automated Linux → UEFI handoff in init script
- [ ] TWRP "Start EDK2" button action modification
- [ ] End-to-end testing on SM-A205W hardware

## Security Notes
- No partitions (boot, recovery, vbmeta) are modified
- No user data is touched
- TWRP kernel/ramdisk/interface unchanged
- All builds via GitHub Actions only
