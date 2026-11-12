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
2. A future TWRP loader will load a dedicated ARM64 Linux Image, matching
   board DTB, and initramfs into RAM. This path is not yet approved or proven.
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
- The device configuration references `prebuilt/kexec/Image` and copies
  `prebuilt/kexec/kexec`, but the kexec executable is absent from that
  repository path and must be built or supplied before TWRP can load a
  second kernel.
- The prebuilt kernel's embedded configuration was extracted and confirms
  `CONFIG_KEXEC=y`, `CONFIG_BLK_DEV_INITRD=y`, `CONFIG_DEVTMPFS=y`,
  `CONFIG_OF=y`, `CONFIG_PROC_FS=y`, and `CONFIG_SYSFS=y`.
- The matching board overlay is
  `exynos7884B-a20_can_single_05.dts`, compatible with
  `Samsung A20 CAN SINGLE 05` and hardware revisions 0x05–0xff.
- GitHub Actions successfully compiles the base DTB and A20 overlay and
  merges them. A separate virtual-boot memory overlay now corrects the
  three audited RAM banks and adds explicit EDK2/Bank 2 carveout reservations.
- The candidate DTB removes the stale fixed initrd addresses and the
  `skip_initramfs` boot argument. A future runtime loader must supply the
  actual initrd start/end addresses.
- A diagnostic BusyBox initramfs and a hashed, non-bootable candidate archive
  have been built in GitHub Actions. They are analysis artifacts only.
- EDK2 is built with
  `EXYNOS7885Pkg/Devices/a205w.dsc`.
- The current EDK2 BootShim is deliberately inert (SAFE MODE).
- The current kernel kexec relocation path clears x0-x3 before branching
  to the target entry. Therefore standard kexec does not yet provide the
  required ARM64 DTB register handoff for this kernel.
- The prebuilt generic DTB originally described only about 2 GiB of usable
  memory and had fixed initrd addresses. The candidate memory overlay
  corrects the audited 3-bank layout, but it has not been tested on-device.
- The candidate DTB still contains the full vendor peripheral tree. A
  minimal, DMA-safe staging device tree and the required device quiescence
  procedure remain unproven.

## Required implementation work

### A. Virtual Linux boot package

- Build a dedicated Image from the pinned kernel source in the same workflow
  as the board DTB, rather than relying on an unproven prebuilt pairing.
- Verify every corrected RAM bank and reserved carveout against the live
  SM-A205W memory audit.
- Prove that the selected kernel can boot as a second-stage kernel before
  adding any device-side launch action.
- Build a diagnostic initramfs with board checks, hash verification,
  memory-map reporting, and a diagnostic shell.
- Package Image, DTB, initramfs, and a hashed manifest. Keep the archive
  explicitly non-bootable until the loader and memory contract are proven.
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
