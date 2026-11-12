# Virtual Boot ARM64 - Validation Report (Phase 1)

## Build Information
- **Build Date**: BUILD_DATE_PLACEHOLDER
- **Git Commit**: GIT_COMMIT_PLACEHOLDER
- **Kernel Source**: https://github.com/toufik01072002/Kernel_SM-A205W
- **Kernel Branch**: kexec-arm64-4.4-d-work
- **Kernel Commit**: 1e70ef871acb9ea0cd79a531b18e98f61debfc71

## Component Status

### ✅ Kernel Configuration (virtual-boot/kernel/config_virtual_boot)
- **Architecture**: ARM64
- **Platform**: Exynos 7885 (CONFIG_SOC_EXYNOS7885=y)
- **KEXEC Support**: Full (CONFIG_KEXEC=y, CONFIG_KEXEC_FILE=y, CONFIG_KEXEC_JUMP=y, CONFIG_KEXEC_ARM64_HIGHMEM=y)
- **Memory**: 4K pages, 48-bit VA/PA, CMA support
- **Status**: Ready for GitHub Actions build

### 🔄 DTB Source (virtual-boot/dtb/exynos7885-a205w-virtual.dts)
- **Source**: Authentic Samsung DTS chain from Kernel_SM-A205W
  - Base: `arch/arm64/boot/dts/exynos/dtbo/exynos7885.dts`
  - Overlay: `arch/arm64/boot/dts/exynos/dtbo/exynos7884B-a20_common.dtsi`
- **Model**: Samsung Galaxy A20 (SM-A205W) - corrected from A20e
- **Memory Regions**: Defined per Samsung DTB (3 banks + Bank 3 empty)
- **Reserved Regions**: All Samsung reservations included (TIMA, seclog, modem, gnss, wifibt, abox, framebuffer, CMA pools)
- **UEFI FD Region**: Added at 0x90000000-0x90200000 (2 MiB) with no-map
- **CPUs**: 8 cores (2x A73 + 6x A53) with correct idle states
- **Peripherals**: UART2 (console), I2C4, MMC0, USB, DECON, ABOX, PWM, RTC, Watchdog, ADC, Thermal
- **Status**: Source created, **PENDING COMPILATION AND BINARY VALIDATION**

### 🔄 DTB Binary Validation
- **Requirement**: Validate compiled DTB binary using libfdt, not regex
- **Checks Required**:
  1. Memory bank reg properties with correct #address-cells/#size-cells
  2. All reserved-memory regions present with correct attributes (no-map, reusable)
  3. No overlapping regions
  4. Reservations contained within RAM banks
  5. /memreserve/ entries from exynos7885-rmem.dtsi
  6. Load addresses compatible with memory map
- **Current Status**: NOT YET VALIDATED - requires successful build first

### 🔄 Initramfs (Defined in build-virtual-boot.yml)
- **Base**: BusyBox 1.36.1 static (ARM64)
- **Init Script**: Diagnostics, hardware info, interactive shell
- **Tools**: hwinfo
- **Filesystem**: proc, sysfs, devtmpfs, tmpfs mounted at boot
- **Status**: Design complete, **PENDING BUILD**

### 🔄 GitHub Actions Workflow (.github/workflows/build-virtual-boot.yml)
- **Job 1**: build-kernel - Clone Kernel_SM-A205W (kexec-arm64-4.4-d-work, commit 1e70ef87), build Image + DTB
- **Job 2**: build-initramfs - Build BusyBox static, create initramfs.cpio.gz
- **Job 3**: build-bundle - Create manifest, SHA256SUMS, documentation
- **Status**: REQUIRES REWRITE with correct:
  - Repository URL
  - Branch and commit
  - DTB compilation path (exynos/dtbo/)
  - Compiler compatible with kernel 4.4
  - Proper dependency installation per job

## Memory Map Validation - REQUIRED CHECKS

### Reference (DeviceMemoryMap.h - commit f4af1d3)
| Region | Address | Size |
|--------|---------|------|
| RAM Bank 0 Low | 0x80000000 | 0x10000000 (256 MiB) |
| UEFI FD | 0x90000000 | 0x00200000 (2 MiB) |
| RAM Bank 0 Mid | 0x90200000 | 0x27E00000 (638 MiB) |
| TIMA Reserved | 0xB8000000 | 0x00200000 (2 MiB) |
| RAM Bank 0 High | 0xB8200000 | 0x03A00000 (58 MiB) |
| seclog Reserved | 0xC0000000 | 0x00080000 (512 KiB) |
| RAM Bank 1 Low | 0xC0080000 | 0x28F80000 (655.5 MiB) |
| WiFi/BT Reserved | 0xE9000000 | 0x00400000 (4 MiB) |
| ABOX Reserved | 0xE9400000 | 0x00400000 (4 MiB) |
| RAM Bank 1 Mid | 0xE9800000 | 0x02800000 (40 MiB) |
| Framebuffer | 0xEC000000 | 0x01400000 (20 MiB) |
| RAM Bank 1 High | 0xED400000 | 0x03800000 (56 MiB) |
| CMA video_stream | 0xF0C00000 | 0x04800000 (72 MiB) |
| Modem/CP Reserved | 0xF5400000 | 0x09800000 (152 MiB) |
| RAM Bank 1 VeryHigh | 0xFEC00000 | 0x01000000 (16 MiB) |
| GNSS Reserved | 0xFFC00000 | 0x00400000 (4 MiB) |
| CMA Pool cma-3 | 0x8BA000000 | 0x01400000 (20 MiB) |
| CMA Pool crypto | 0x8BB800000 | 0x02000000 (32 MiB) |
| CMA Pool video_frame | 0x8BD800000 | 0x02800000 (40 MiB) |
| RAM Bank 2 Low | 0x880000000 | 0x3A000000 (928 MiB) |
| RAM Bank 2 Gap1 | 0x8BB400000 | 0x00400000 (4 MiB) |

### Samsung DTB Reserved Regions (exynos7885-rmem.dtsi + A20 overlay)
- framebuffer@0xEC000000: 0x1400000 (20 MiB)
- modem_if@0xF5400000: 0x8800000 (136 MiB)
- cp_ram_logging@0xFDC00000: 0x2000000 (32 MiB)
- gnss_if@0xFFC00000: 0x400000 (4 MiB)
- tima_region@0xB8000000: 0x200000 (2 MiB)
- seclog@0xC0000000: 0x80000 (512 KiB)
- wifibt_if@0xE9000000: 0x400000 (4 MiB)
- abox_rmem@0xE9400000: 0x400000 (4 MiB)
- btabox_rmem: 0x400000 (4 MiB) - dynamic allocation
- crypto (CMA): 0x2000000 (32 MiB) - reusable
- video_stream (CMA): 0x4800000 (72 MiB) - secure, reusable

### Validation Status: **PENDING**
The current regex-based validator is insufficient. A proper libfdt-based binary DTB validator must be created and run after successful compilation.

## Kernel Branch Selection Analysis

### kexec-arm64-4.4-d (c830e5fa)
- Base import from Samsung opensource
- No KEXEC fixes

### kexec-arm64-4.4-d-work (1e70ef87) **SELECTED**
Contains 14 commits ahead of base:
1. 633c7337: ARM64: add KEXEC support for Exynos7885
2. 2b0a5fc0: ARM64: fix KEXEC build and UH ELF path
3. c3f30255: CI: add A20 ARM64 KEXEC build workflow
4. fc36fe3d: CI: expose Android version to all build steps
5. 6405c274: CI: fix DTC host build with GCC common symbols
6. b59968af: ARM64: include const definitions in page header
7. 2bad81ea: Fix embedded firmware paths for out-of-tree build
8. 4b3687ef: Fix missing Netfilter xt_mark header
9. d0ac1826: Fix missing Netfilter xt_dscp header
10. 887532cc: Fix missing IPv4 TTL Netfilter header
11. 1e70ef87: Fix missing IPv6 hop limit Netfilter header

**Rationale**: The work branch contains explicit KEXEC support for Exynos7885 and A20 build workflow, making it the correct choice for Virtual Boot.

## Outstanding Issues (Must Fix Before Phase 1 Complete)

1. **DTB Compilation**: Must compile using authentic Samsung DTS chain from `arch/arm64/boot/dts/exynos/dtbo/`
2. **Binary DTB Validation**: Replace regex validator with libfdt-based validation
3. **Workflow Rewrite**: Fix repository URL, branch, commit, compilation paths, compiler
4. **Initramfs Build**: Install aarch64-linux-gnu-gcc in build job, verify BusyBox ARM64 static build
5. **Memory Address Assignment**: DTB at 0x88000000 collides with trampoline (Phase 2) - document clearly
6. **No Placeholders**: Manifest must contain real SHA256, real commit, real build date
7. **Model Correction**: Galaxy A20 (not A20e) throughout all documentation

## Next Steps (Order Mandatory)

A. ✅ Compare kernel branches - COMPLETE (kexec-arm64-4.4-d-work selected)
B. ✅ Identify authentic DTS/DTBO chain - COMPLETE
C. 🔄 Correct DTS and validate compilation - IN PROGRESS
D. 🔄 Validate memory from binary DTB - PENDING
E. 🔄 Correct kernel config and build workflow - PENDING
F. 🔄 Build initramfs with proper dependencies - PENDING
G. 🔄 Generate bundle, hashes, reports - PENDING

## Phase 2 Boundary
This report covers Phase 1 only. The following are explicitly NOT part of Phase 1:
- Linux → UEFI trampoline execution
- TWRP button integration
- UEFI handoff automation
- EDK2 bundle loading
