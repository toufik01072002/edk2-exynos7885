# A20 virtual boot (development)

## Scope

This directory defines the RAM-only second-stage boot environment for the
Samsung SM-A205W (Exynos 7885). It is loaded from TWRP with the existing
ARM64 kexec path. It must not write to the boot, recovery, or vbmeta
partitions.

This is a development contract, not a claim that the EDK2 handoff is already
implemented.

## Intended flow

1. TWRP verifies the package manifest and hashes.
2. TWRP loads the dedicated ARM64 Linux Image, matching DTB, and initramfs
   into RAM through kexec.
3. The virtual-boot initramfs verifies the board identity, package contents,
   memory reservations, and required devices.
4. A kernel-supported handoff mechanism prepares the machine for EDK2.
5. The handoff trampoline enters the EDK2 SEC/PrePi entry point with a
   documented register, stack, exception, cache, MMU, and interrupt state.

## Non-negotiable safety rules

- No writes to boot, recovery, or vbmeta.
- No guessed physical load address and no assumption that a RAM range is free.
- Reserve the firmware, trampoline, stack, DTB, and initramfs ranges before
  they can be overwritten by the running kernel or DMA-capable devices.
- Reject a mismatched board, invalid DTB, bad hash, overlapping memory range,
  unsupported CPU state, or missing handoff support.
- Keep the TWRP launch action fail-closed until the handoff is implemented
  and validated on the exact A205W kernel.
- Build all kernel and firmware artifacts in GitHub Actions; do not compile
  them in Termux.

## Current integration facts

- Device target: Samsung SM-A205W, ARM64, Exynos 7885 family.
- TWRP device configuration already references
  `prebuilt/kexec/Image`.
- The device configuration copies a kexec executable and
  `exynos7885.dtb` into recovery.
- EDK2 is built with
  `EXYNOS7885Pkg/Devices/a205w.dsc`.
- The current EDK2 BootShim is deliberately inert (SAFE MODE).
- The current kernel kexec relocation path was found to clear x0-x3 before
  branching to the target entry. Therefore it is not yet a proven Linux
  ARM64 DTB handoff, and it is not an EDK2 handoff.

## Required implementation work

### A. Virtual Linux boot package

- Pin the exact kernel commit and build configuration.
- Build a dedicated Image with the required kexec and initramfs support.
- Pin and validate the matching A205W DTB.
- Build a small initramfs with board checks, hash verification, memory-map
  validation, and a diagnostic shell.
- Package Image, DTB, initramfs, EDK2 bundle, and a signed or hashed manifest.
- Add a reproducible GitHub Actions workflow and artifact verification.

### B. Kernel handoff support

- Define an explicit handoff ABI for the EDK2 entry point.
- Pass the DTB pointer and boot metadata through a documented structure.
- Implement the handoff in kernel-controlled code, not from an ordinary
  userspace process.
- Stop secondary CPUs and quiesce interrupt, timer, storage, display, and
  DMA-capable devices as required.
- Establish the exact EL, MMU, cache, TLB, exception, DAIF, and stack state.
- Keep a reserved-memory description consistent between the device tree,
  kernel, TWRP loader, and EDK2 memory map.
- Add static checks and runtime diagnostics; do not enable execution until
  the contract is satisfied.

### C. TWRP integration

- Verify package format, target identity, component sizes, and hashes.
- Verify that all destination RAM ranges are available and non-overlapping.
- Load only the virtual Linux boot package into RAM.
- Report each failed precondition and return to TWRP without attempting a
  partial handoff.
- Enable the EDK2 action only after the complete chain is validated.

## Acceptance criteria

The integration is not complete until all of the following are demonstrated:

1. Reproducible GitHub Actions build of the virtual boot package.
2. Correct DTB and initramfs delivery to the virtual kernel.
3. Runtime proof of the reserved physical memory ranges.
4. A tested kernel handoff ABI with the required CPU state.
5. EDK2 reaches its first PEI/PrePi diagnostic on the SM-A205W.
6. Failure at any validation step leaves TWRP usable and does not write
   boot, recovery, or vbmeta.

Until these criteria are met, the package must remain non-launchable from
the normal TWRP button.
