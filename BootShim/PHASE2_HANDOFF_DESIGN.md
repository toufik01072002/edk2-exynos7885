# Phase 2 — Linux to UEFI handoff design

## Status

**Design and source review only. No handoff implementation is validated yet.**
The Phase 1 kernel, DTB, initramfs and bundle build passed in GitHub Actions run 68. That result does not validate BootShim, the EDK2 firmware entry point, or a Linux-to-UEFI transfer.

## Source evidence at baseline commit 2786c36d2d33026fcb9af53be140953a30db38f2

- `BootShim/BootShim.S` contains a 64-byte ARM64 Linux Image-style header, then a copy routine that copies `UEFI_SIZE` bytes from `_Payload` to `UEFI_BASE` and branches to `UEFI_BASE`.
- The header currently writes `UEFI_BASE` (0x90000000) into the `text_offset` field and `UEFI_SIZE` (0x00200000) into `image_size`.
- Linux ARM64 boot documentation in the selected kernel branch describes `text_offset` as the image load offset from a 2 MiB-aligned RAM base. The same branch's `arch/arm64/kernel/head.S` rejects `TEXT_OFFSET > 0x1fffff`. Therefore 0x90000000 is not a valid ARM64 Image `text_offset`; the kexec-loadable wrapper header must be redesigned rather than treated as a finished Linux Image.
- `_Payload` is a label at the end of the BootShim code. The firmware is appended by the top-level `build.sh`: it concatenates `BootShim/BootShim.bin` and `workspace/Build/EXYNOS7885Pkg/DEBUG_GCC/FV/EXYNOS7885PKG_UEFI.fd` into `workspace/UEFI`. Thus the FD payload is present after the shim in that combined image; the assembly source itself does not use `.incbin`.
- `build_bootshim.sh` builds only the shim binary; the top-level `build.sh` performs the subsequent shim+FD concatenation and wraps the combined image with `mkbootimg` as `boot.img`.
- The EDK2 workflow `.github/workflows/build-a205w.yml` runs `build.sh` and publishes `workspace/UEFI` plus boot/recovery images. It is separate from the Virtual Boot workflow. Run 68 validates the Virtual Boot kernel/DTB/initramfs bundle, not this BootShim+FD combination.
- `EXYNOS7885Pkg/EXYNOS7885Pkg.fdf` defines an EDK2 FD at base 0x90000000 with size 0x00200000. `DeviceMemoryMap.h` reserves the same 2 MiB as `BootShim/FD`, and the Virtual Boot DTB marks that range no-map.
- The selected kernel branch's `arch/arm64/kernel/machine_kexec.c` includes a guard that BUGs if more than one CPU is online for a normal kexec. SMP shutdown requirements must therefore be resolved explicitly before any launch attempt.

## Phase 2 progress (2026-11-13)

- The FD packaging gate is integrated in CI and passed in EDK2 build run 19.
- The BootShim now emits the 64-byte ARM64 Image header with a provisional, protocol-valid 0x80000 text offset; the finalizer writes image_size to the complete concatenated BootShim+FD length.
- CI checks the ARM64 magic, text_offset, image_size, flags, FD window and exact concatenation.
- The current assembly deliberately stops in a WFE loop. It does not copy the FD or branch to 0x90000000. This is an intentional safety gate until the EDK2 entry point and complete handoff ABI are established.
- These checks are static/package validation only. They do not yet prove the image can be loaded by the target kernel's kexec path.

## Phase 2 work sequence

### 2.1 Define and validate the firmware input
1. Identify the exact EDK2 FD artifact produced by the EDK2 build and its reproducible path.
2. Verify its format, base-address expectation, and size against the FDF definition.
3. Require the FD payload to fit entirely in the reserved 2 MiB window. Do not silently truncate or pad an oversized image.
4. Record the FD SHA-256 and exact EDK2 source commit in the bundle manifest.

### 2.2 Correct the kexec-loadable wrapper
1. Implement a valid ARM64 Linux Image header for the BootShim wrapper. `text_offset`, `image_size`, flags, alignment and actual binary length must agree with the selected kernel's ARM64 boot protocol and kexec loader.
2. Keep the existing shim+FD concatenation, but make the payload boundary explicit and validate it: the FD must begin exactly at the end of BootShim.bin, with a recorded offset and length. The wrapper's Linux Image header must declare the complete combined image size, not merely the 2 MiB FD size.
3. Validate that the kexec loader places the wrapper in usable RAM, not in the reserved FD window, and that the source payload cannot overlap the 0x90000000 destination.
4. Add static checks for header fields, payload offset/length, FD bounds, destination range and SHA-256.

### 2.3 Implement the handoff contract
Before branching to firmware, the implementation must document and satisfy:
- the exact EDK2 entry address and calling convention (do not assume the FD base is automatically the executable entry);
- the FDT address and register contract, including preservation or deliberate translation of x0;
- exception level and security state expected by the EDK2 entry;
- MMU/cache state, cache clean/invalidate operations for copied firmware, and required DSB/ISB barriers;
- interrupt masking and architected system-register state;
- secondary CPU quiescence and the kernel branch's multi-CPU kexec restriction;
- DMA-capable device quiescence and a deterministic failure path before control is transferred.

The current copy-and-branch assembly does not yet demonstrate these conditions. Do not add a blind `br 0x90000000` launch path.

### 2.4 CI and validation gates
1. Add a dedicated GitHub Actions job to build BootShim with the verified FD input.
2. Fail the build on invalid ARM64 Image header fields, missing payload, incorrect FD size, overlap, or checksum mismatch.
3. Produce a combined handoff artifact and a manifest containing component hashes, offsets, lengths, and source commits.
4. Add host-side tests for the header and copy layout before considering emulator or hardware validation.
5. Keep the TWRP button integration out of scope until the handoff contract and artifact checks pass.

## Safety boundary

- No flashing or writing to the phone's boot, recovery, or vbmeta partitions.
- No on-device launch or reboot test is authorized by this design document.
- A successful CI build or static validation is not proof that EDK2 starts on the SM-A205W.
- Any future device test requires a separately reviewed procedure and explicit user approval.

## Exact target-kernel kexec findings (2026-11-13)

Reviewed directly from `toufik01072002/Kernel_SM-A205W`, branch
`kexec-arm64-4.4-d-work`, commit
`1e70ef871acb9ea0cd79a531b18e98f61debfc71`.

### Confirmed kernel behavior

- `arch/arm64/kernel/Makefile` links `machine_kexec.o` and
  `relocate_kernel.o` under `CONFIG_KEXEC`. It does not link an
  ARM64 Image-format `kexec_image.o` or `machine_kexec_file.o`.
  Therefore this branch's kernel-side implementation is the legacy
  `kexec_load()` segment path, not an in-kernel ARM64 Image parser /
  `kexec_file_load()` path. The userspace kexec-tools loader and its exact
  version remain to be identified before the wrapper header can be declared
  accepted.
- `machine_kexec_prepare()` only stores `kimage->start` in
  `kimage_start` and records debug information. It does not parse the
  ARM64 Image header, validate `text_offset`, or derive an entry point.
- `machine_kexec()` contains
  `BUG_ON((num_online_cpus() > 1) && !WARN_ON(in_crash_kexec))`.
  For an ordinary (non-crash) kexec, more than one online CPU therefore
  triggers a kernel BUG. A safe launch requires a proven CPU-quiescence /
  single-online-CPU path before entering this function; the current TWRP
  button path does not establish that.
- The kernel copies `arm64_relocate_new_kernel` into a dedicated control
  page, flushes the relocation code and image data caches, masks DAIF,
  calls `setup_mm_for_reboot()`, then enters the relocation routine through
  `cpu_soft_restart()`. The reset helper clears SCTLR_EL1 flags and, when
  required, uses the EL2 HVC transition.
- `relocate_kernel.S` receives the entry address in x1, moves it to x18,
  performs the kexec segment relocation, executes DSB / I-cache invalidate /
  DSB / ISB, then explicitly sets x0, x1, x2 and x3 to zero before
  `br x18`. Consequently, the Linux FDT pointer is **not passed in x0**
  to the BootShim by this kernel's ordinary kexec path. A future shim must
  obtain a DTB through a separately designed, validated handoff record or
  a reviewed kernel/userspace change; it must not assume x0 contains the
  recovery DTB.
- The target-kernel `arch/arm64/include/asm/boot.h` defines a 2 MiB
  minimum kernel-image alignment and an 8-byte minimum FDT alignment,
  with a 2 MiB maximum FDT size. The ARM64 startup header in `head.S`
  rejects `TEXT_OFFSET > 0x1fffff` and documents MMU-off / D-cache-off
  requirements for a native Linux kernel entry. These constraints do not
  by themselves define the EDK2 entry ABI.

### Consequences for the current BootShim

1. The current 0x80000 `text_offset` remains a protocol-format candidate,
   not proof that this exact kexec-tools binary accepts the concatenated
   BootShim+FD image or reserves the full `image_size` range.
2. The current shim may execute with x0-x3 all zero. The existing WFE gate
   remains mandatory until the DTB transport, EDK2 entry address, EL,
   system-register state, cache policy, and CPU shutdown procedure are
   explicitly implemented and tested.
3. The EDK2 FD base (0x90000000) is a firmware placement address, not
   evidence that the FD's first byte is a callable firmware entry point.
   The EDK2 PE/COFF or platform entry symbol and required entry state must
   be established from the built firmware and platform source.
4. Before changing assembly, inspect the actual userspace `kexec` binary
   used by the TWRP launch flow (source/version and ARM64 loader path),
   then test its image-size and segment-placement behavior in CI or a
   host-side harness. Do not infer this from the kernel's
   `machine_kexec_prepare()` because that function does not parse the
   Image header.

## PrePi entry contract and physical-placement blocker (2026-11-13)

### Artifact-backed SEC entry

Build #29 produced and passed the static PrePi SEC TE parser. Its manifest
records:

- FD linked base: `0x90000000`, length: `0x200000`.
- SEC TE image base: `0x90001000`.
- SEC entry RVA: `0x7280`.
- SEC entry address: `0x90008280`.
- SEC entry file offset in the FD: `0x8280`.
- First instruction bytes: `e6 01 00 94` (AArch64 `BL`).
- Decoded branch destination: `0x90008A18`.

The CI parser now checks that the entry point lies in an executable TE section,
that its XIP address matches its FD file offset, and that its first BL target
remains inside that executable section. This is static validation, not proof
that the CPU can execute the entry at runtime.

### Actual PrePi entry ABI

The built `PrePi/AArch64/ModuleEntryPoint.S` does not consume an incoming DTB
pointer. It calls `ArmPlatformPeiBootAction`, calculates its own stack and UEFI
memory region from PCD values, reads MPIDR with `ArmReadMpidr`, then calls
`CEntryPoint(MpId, UefiMemoryBase, StacksBase)`. Thus the PrePi C entry gets
MPIDR in x0, not the Linux DTB pointer. The current platform library's
`ArmPlatformInitialize()` is a no-op returning success, and the DSC sets
`PcdCoreCount=1`. The present SEC code is not an FDT handoff consumer.

For the current A205W DSC:
- System RAM base: `0x80000000`.
- First contiguous system-memory size: `0x38000000`, ending at
  `0xB8000000` (exclusive).
- SEC/PEI UEFI region size: `0x03000000` (48 MiB).
- FD link base/range: `0x90000000-0x90200000`.

With the FD below the top of this memory range, the assembly's stack-selection
logic chooses the top of system memory, and places the UEFI region immediately
below it. The exact primary stack size remains controlled by the resolved
`PcdCPUCorePrimaryStackSize` value and must be included in the handoff review.

### New blocker: FD link address versus standard ARM64 Image placement

The current concatenation places the FD at byte offset `0x50` (80 bytes)
inside the ARM64 Image wrapper. The header advertises `text_offset=0x80000`.
For the standard kexec-tools ARM64 Image loader, the image segment is placed
at an address of the form:

`aligned_2MiB_hole + text_offset`

and the FD therefore starts at:

`aligned_2MiB_hole + 0x80000 + 0x50`

The offset from the aligned 2 MiB hole is `0x80050`, which is not 2 MiB
aligned. But the EDK2 FD is linked for the fixed 2 MiB-aligned address
`0x90000000`. Therefore **no 2 MiB-aligned hole can make the current
80-byte-prefix bundle's FD land at its linked base under that standard
placement rule**. The current successful CI package does not detect this
physical-address mismatch.

The upstream kexec-tools ARM64 loader reference uses a 2 MiB-aligned hole and
adds `text_offset` to the kernel segment address before loading the image:
https://kernel.googlesource.com/pub/scm/utils/kernel/kexec/kexec-tools/+/refs/heads/main/kexec/arch/arm64/kexec-arm64.c
See `arm64_locate_kernel_segment()` and the ARM64 image-loading path. This
reference is not yet proof of the exact kexec-tools binary (or loader mode)
present in the user's TWRP image; that binary and version must be identified.

Do not fix this by merely changing `text_offset` or padding the shim. The
selected physical hole is dynamic, and the EDK2 TE image is linked at an
absolute base. First identify the actual TWRP kexec executable/version and
loading path. Then choose and implement one explicit placement strategy:
fixed-address kexec segments, a validated copy/relocation of the FD into its
linked address with a reserved non-overlapping destination, or a genuinely
relocatable EDK2 SEC image. Add a CI test for the chosen address equation.

Until that is resolved, the SEC entry address `0x90008280` is an artifact
address only. The WFE handoff gate must remain in place.


## Exact TWRP kexec-tools loader audit (2026-11-13)

The userspace kexec implementation is now identified from the device-tree build
workflow and the exact upstream source used by that workflow.

### TWRP packaging evidence

- Device-tree repository:
  `toufik01072002/android_device_samsung_a20`, branch `android-11`,
  audited head `d3d504eb42df66653d38bfc7e0da6b64e4c3932f`.
- Build workflow: `.github/workflows/twrp-a20-build.yml`.
- The workflow downloads the official `kexec-tools-2.0.32.tar.xz`, builds
  for AArch64 Android with NDK `27.2.12479018`, and installs the binary as
  `prebuilt/kexec/kexec`.
- `device.mk` copies that binary to `/system/bin/kexec` in the recovery
  ramdisk. The same packaging rule installs
  `prebuilt/kexec/exynos7885.dtb` at
  `/system/etc/kexec/exynos7885.dtb`.
- The exact upstream release tag resolves to commit
  `15d78e5799eea7ec5ea9c5897ae95aaa0ce8970c`
  (`horms/kexec-tools`, tag `v2.0.32`).
- Modified recovery repository:
  `toufik01072002/android_bootable_recovery`, branch `edk2-integration`,
  audited head `8074981c47302e56ccdddeab68bd7a013317b803`.
  Its `apply_edk2_ui.py` currently adds an explicitly disabled status page;
  that script does not execute `/system/bin/kexec`. The actual launcher
  command and argument list therefore remain unimplemented/unverified.

### Exact ARM64 Image loader behavior in v2.0.32

Reviewed from the release-tagged sources:

- `kexec/arch/arm64/kexec-arm64.c`:
  `arm64_process_image_header()` reads `text_offset` and `image_size`
  from the 64-byte ARM64 Image header when `image_size` is nonzero.
- `arm64_locate_kernel_segment()` asks `locate_hole()` for a region of
  `text_offset + image_size`, with `MiB(2)` alignment.
- `kexec/arch/arm64/kexec-image-arm64.c` then loads the image segment at
  `kernel_segment + text_offset`, with `image_size` as the segment memory
  size.
- `kexec/kexec.c` defaults to trying `kexec_file_load` first and allows
  fallback. When the file syscall is unavailable (including `ENOSYS`), it
  falls back to the userspace image loader and legacy `kexec_load`.
  The explicit `--kexec-file-syscall` option disables that fallback.
- The reviewed A205W kernel branch
  `Kernel_SM-A205W:kexec-arm64-4.4-d-work` does not implement the ARM64
  `kexec_file_load` path, so the expected compatible route is the
  kexec-tools userspace ARM64 loader followed by legacy `kexec_load`.
  The exact runtime command still must not be assumed until the launcher is
  implemented and its arguments are reviewed.

### Confirmed placement equation and consequence

For the current BootShim package:

- `text_offset = 0x80000`.
- The FD begins at byte offset `0x50` in the combined image.
- The legacy userspace loader places the combined image at
  `aligned_2MiB_hole + text_offset`.
- Therefore the FD begins at
  `aligned_2MiB_hole + 0x80050`, not at the EDK2 linked base
  `0x90000000`.

Because the selected hole is 2 MiB aligned, this loader cannot place the
current FD directly at `0x90000000`. This confirms the physical-placement
blocker from the prior section using the exact TWRP kexec-tools release rather
than only upstream development-branch code.

No assembly handoff change is authorized by this finding. Keep the WFE gate.
The next implementation decision must be one of: a fixed-address segment
loader, a carefully bounded copy/relocation of the FD into
`0x90000000-0x90200000` after proving that destination is reserved and does
not overlap live memory, or a genuinely relocatable EDK2 SEC build. The
chosen strategy needs a CI address-equation test before any branch-to-firmware
code is enabled.

### Source references

- Device build workflow:
  https://github.com/toufik01072002/android_device_samsung_a20/blob/d3d504eb42df66653d38bfc7e0da6b64e4c3932f/.github/workflows/twrp-a20-build.yml
- Recovery launcher integration:
  https://github.com/toufik01072002/android_bootable_recovery/blob/8074981c47302e56ccdddeab68bd7a013317b803/apply_edk2_ui.py
- kexec-tools ARM64 loader:
  https://github.com/horms/kexec-tools/blob/15d78e5799eea7ec5ea9c5897ae95aaa0ce8970c/kexec/arch/arm64/kexec-arm64.c
- kexec-tools ARM64 Image segment:
  https://github.com/horms/kexec-tools/blob/15d78e5799eea7ec5ea9c5897ae95aaa0ce8970c/kexec/arch/arm64/kexec-image-arm64.c
- kexec-tools syscall selection/fallback:
  https://github.com/horms/kexec-tools/blob/15d78e5799eea7ec5ea9c5897ae95aaa0ce8970c/kexec/kexec.c


## Linux-side FD destination reservation audit (2026-11-13)

The EDK2 platform memory map reservation is not sufficient to protect the FD
destination while Linux is running. The active Linux kernel must reserve the
physical range independently before a shim can safely copy firmware there.

Source review of the A20 kernel branch
`Kernel_SM-A205W:kexec-arm64-4.4-d-work` found:

- The Exynos7885 base DTS declares RAM at
  `0x80000000-0xBDA00000`; therefore
  `0x90000000-0x90200000` is inside the general RAM range.
- The A20 overlay
  `exynos7884B-a20_common.dtsi` includes
  `exynos7884-rmem_7884.dtsi`. That reserved-memory include only declares
  the ABOX region at `0xE9400000-0xE9800000`; it does not reserve the
  UEFI FD window.
- The Exynos7885 SoC reserved-memory include also has no
  `0x90000000-0x90200000` reservation.
- The EDK2 `DeviceMemoryMap.h` reservation and the Virtual Boot DTB
  `no-map` entry describe the firmware/Virtual Boot environment; they do
  not prove that the currently booted TWRP Linux kernel has reserved this
  range.
- The A20 TWRP device tree points `TARGET_PREBUILT_KERNEL` at
  `prebuilt/kexec/Image` and uses a prebuilt `recoverydtbo`. The binary
  prebuilt DTBO and the exact runtime DTB have not yet been decoded and
  audited, so absence from the DTS source is not proof of the final DTB's
  contents.

**Current conclusion:** do not copy or branch into the FD window yet. The
Linux-side reservation is unproven and the source DTS currently does not
declare it. The next audit must decode the exact DTB/DTBO pair packaged into
the TWRP recovery, resolve the final merged memory map, and verify that the
range is reserved before kexec loads any segments. If it is not reserved,
the kernel/DTB build must be updated and validated in CI before any handoff
code is considered. Do not infer safety from the EDK2 memory map alone.
