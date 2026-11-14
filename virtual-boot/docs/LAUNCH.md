# Virtual Boot ARM64 - Launch Documentation (Phase 1)

## Overview

This package contains the Phase 1 diagnostic Linux environment for Samsung SM-A205W (Galaxy A20, Exynos 7885).

**Phase 1:** kernel, DTB and diagnostic initramfs are built and packaged by GitHub Actions.
**Phase 2:** TWRP integration and the Linux-to-UEFI handoff are not implemented.

## Hardware

- Model: Samsung Galaxy A20 (SM-A205W)
- Platform: Exynos 7885
- Codename: a20
- Architecture: AArch64

## Bundle contents

- `Image` — standalone ARM64 Linux kernel with KEXEC support
- `exynos7885-a205w-virtual.dtb` — Virtual Boot device tree
- `initramfs.cpio.gz` — BusyBox diagnostic userspace
- `hardware-manifest.json` — build identity, hashes and proposed memory map
- `SHA256SUMS` — integrity checks for the bundle
- `patches/` — kernel compatibility patches used for this build

## Build #97

The successful GitHub Actions run #97 produced the bundle from kernel commit `1e70ef871acb9ea0cd79a531b18e98f61debfc71`.

The bundle is a build artifact, not a hardware boot validation. A successful compile and DTB structural validation do not prove that the memory map, peripheral shutdown, or handoff sequence is safe on the phone.

## Launch status — BLOCKED

**Do not run `kexec -l` or `kexec -e` on the SM-A205W with this bundle.**

The end-to-end launch path is not validated. In particular:

- The kernel's ordinary ARM64 kexec entry path does not guarantee that the incoming DTB pointer survives in x0-x3.
- The current BootShim is deliberately fail-closed and loops in WFE; it does not enter EDK2.
- The trampoline remains a fail-closed WFE stub with no assigned physical load address; no runtime placement or entry sequence is implemented.
- The exact physical placement and executable SEC entry point of the EDK2 firmware image have not been proven together.
- Secondary CPU shutdown, GIC state, DMA-capable device quiescence, cache maintenance and exception-level transition still require a reviewed implementation.
- The DTB binary passes structural checks, including the three expected SM-A205W RAM-bank ranges and fixed UEFI reservation; this is not proof of runtime hardware compatibility.

The TWRP action **Démarrer EDK2** must remain blocked until these issues have a tested implementation. Do not bypass the fail-closed guard.

The TWRP userspace loader is currently a launch gate only: it contains no `kexec_load` or reboot syscall path. CI compiles it and verifies that it refuses launch. This prevents a build-time define from re-enabling the old unvalidated prototype.

## Proposed memory addresses

The manifest currently records these proposed addresses:

| Component | Address |
|---|---:|
| Virtual Boot kernel | `0x80080000` |
| Initramfs | `0x84000000` |
| DTB | `0x88000000` |
| UEFI firmware volume reservation | `0x90000000–0x90200000` |
| Trampoline | Not assigned (no physical VMA) |

These are not an authorization to load or execute the components. The DTB address and the old trampoline linker address collide; the layout must be redesigned before any launch attempt.

## Integrity verification

On a host or CI runner, extract the bundle and run:

```sh
sha256sum -c SHA256SUMS
```

This verifies file integrity only. It does not validate the boot path.

## Next engineering gates

1. Define one non-overlapping physical layout for kernel, initramfs, DTB, trampoline and EDK2 firmware.
2. Implement a handoff that receives explicit physical addresses rather than assuming x0-x3 survive kexec.
3. Prove the required CPU, interrupt-controller, DMA and cache shutdown sequence for this Exynos platform.
4. Build and statically validate the trampoline and BootShim in GitHub Actions.
5. Keep the TWRP launch action fail-closed until hardware evidence validates the complete chain.

No phone partitions are modified by these build steps. No flash or on-device execution is authorized by this bundle.
