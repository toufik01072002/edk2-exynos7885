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
- **Source**: Virtual Boot-specific DTS derived from Samsung Exynos7885/A20 hardware definitions
  - Reference base: `arch/arm64/boot/dts/exynos/dtbo/exynos7885.dts` (reviewed, not directly included)
  - Reference A20 overlay: `arch/arm64/boot/dts/exynos/dtbo/exynos7884B-a20_common.dtsi` (reviewed, not directly included)
  - Direct Samsung include: `exynos7885-pm-domains.dtsi`
  - The custom DTS intentionally selects and adapts the hardware nodes needed for the diagnostic kernel; it is not a verbatim merge of the complete base DTS and DTBO overlay.
- **Model**: Samsung Galaxy A20 (SM-A205W) - corrected from A20e
- **Memory Regions**: Defined per Samsung DTB (3 banks + Bank 3 empty)
- **Reserved Regions**: All 13 fixed EDK2 MEM_RES descriptors are mirrored as no-map reservations in the Virtual Boot DTB
- **UEFI FD Region**: Added at 0x90000000-0x90200000 (2 MiB) with no-map
- **CPUs**: 8 cores (2x A73 + 6x A53) with correct idle states
- **Peripherals**: UART2 (console), I2C4, MMC0, USB, DECON, ABOX, PWM, RTC, Watchdog, ADC, Thermal
- **Status**: Source created, **PENDING COMPILATION AND BINARY VALIDATION**

### 🔄 DTB Binary Validation
- **Requirement**: Validate the compiled DTB binary with `dtc`/`fdtget` (not regex) against `DeviceMemoryMap.h`
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
- **Status**: Workflow corrections published; first successful GitHub Actions run still pending.
  - Kernel checkout exposes the exact commit output.
  - DTS is preprocessed with the kernel include paths before `dtc` compilation.
  - Kernel artifacts are staged at stable paths before upload.
  - Binary DTB validation receives both the DTB and `DeviceMemoryMap.h`.
  - Initramfs and bundle jobs still require execution evidence.

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
| Modem Reserved | 0xF5400000 | 0x08800000 (136 MiB) |
| CP RAM Logging | 0xFDC00000 | 0x02000000 (32 MiB) |
| GNSS Reserved | 0xFFC00000 | 0x00400000 (4 MiB) |
| CMA Pool cma-3 | 0x8BA000000 | 0x01400000 (20 MiB) |
| CMA Pool crypto | 0x8BB800000 | 0x02000000 (32 MiB) |
| CMA Pool video_frame | 0x8BD800000 | 0x02800000 (40 MiB) |
| RAM Bank 2 Low | 0x880000000 | 0x3A000000 (928 MiB) |
| RAM Bank 2 Gap1 | 0x8BB400000 | 0x00400000 (4 MiB) |

### Virtual Boot Fixed Reservations
- Framebuffer: 0xEC000000-0xED400000 (20 MiB)
- Modem: 0xF5400000-0xFDC00000 (136 MiB)
- CP RAM logging: 0xFDC00000-0xFFC00000 (32 MiB)
- GNSS: 0xFFC00000-0x100000000 (4 MiB)
- TIMA: 0xB8000000-0xB8200000 (2 MiB)
- seclog: 0xC0000000-0xC0080000 (512 KiB)
- WiFi/BT: 0xE9000000-0xE9400000 (4 MiB)
- ABOX: 0xE9400000-0xE9800000 (4 MiB)
- CMA video_stream: 0xF0C00000-0xF5400000 (72 MiB)
- CMA cma-3: 0x8BA000000-0x8BB400000 (20 MiB)
- CMA crypto: 0x8BB800000-0x8BD800000 (32 MiB)
- CMA video_frame: 0x8BD800000-0x8C0000000 (40 MiB)
- All 13 fixed reservations are represented as `no-map` regions in the Virtual Boot DTS.
- The stock Samsung dynamic CMA/ABOX allocations are intentionally pinned or omitted in this diagnostic-only DTB so they cannot consume memory reserved by EDK2.

### Validation Status: **PENDING**
The binary validator now uses `dtc`/`fdtget`, checks the FDT header and all three `/memory` banks, verifies every fixed reservation is inside RAM and carries `no-map`, and requires exact parity between fixed DTB reservations and all `MEM_RES` descriptors. It has not yet been executed against a compiled DTB.

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
3. **First CI execution**: Run the corrected workflow and fix the first real compiler or validation failure
4. **Initramfs Build**: Install aarch64-linux-gnu-gcc in build job, verify BusyBox ARM64 static build
5. **Memory Address Assignment**: DTB at 0x88000000 collides with trampoline (Phase 2) - document clearly
6. **No Placeholders**: Manifest must contain real SHA256, real commit, real build date
7. **Model Correction**: Galaxy A20 (not A20e) throughout all documentation

## Next Steps (Order Mandatory)

A. ✅ Compare kernel branches - COMPLETE (kexec-arm64-4.4-d-work selected)
B. ✅ Identify authentic DTS/DTBO chain - COMPLETE
C. 🔄 Correct DTS and DTB compilation inputs - PREPARED, BUILD PENDING
D. 🔄 Validate memory from binary DTB - VALIDATOR IMPLEMENTED, EXECUTION PENDING
E. ✅ Correct kernel config checks and build workflow - CORRECTIONS PUBLISHED
F. 🔄 Build initramfs with proper dependencies - PENDING
G. 🔄 Generate bundle, hashes, reports - PENDING

## Phase 2 Boundary
This report covers Phase 1 only. The following are explicitly NOT part of Phase 1:
- Linux → UEFI trampoline execution
- TWRP button integration
- UEFI handoff automation
- EDK2 bundle loading
