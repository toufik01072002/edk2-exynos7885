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


## TWRP kexec binary evidence (2026-11-13)

The TWRP workflow source and successful build run 26 were inspected:

- TWRP device repository: `toufik01072002/android_device_samsung_a20`, branch
  `android-11`, commit `d3d504eb42df66653d38bfc7e0da6b64e4c3932f`.
- `.github/workflows/twrp-a20-build.yml` downloads the upstream
  `kexec-tools-2.0.32.tar.xz` release from kernel.org and builds it for
  Android ARM64 with Android NDK `27.2.12479018`, Clang target
  `aarch64-linux-android24`, and PIE flags.
- Successful TWRP run 26 (run ID `43109123257`) produced
  `prebuilt/kexec/kexec` as a stripped, dynamically linked AArch64 ELF PIE
  using interpreter `/system/bin/linker64`.
- The exact binary SHA-256 printed by that run was
  `a1d11d8be9caad87dfd5289588468470ab3cff9a6c71f73f53b4ca22abca018d`.
- The build log confirms the binary was copied into
  `out/target/product/a20/recovery/root/system/bin/kexec`; the device
  `device.mk` maps the generated prebuilt into `/system/bin/kexec`.
  The binary is generated during CI and is not committed in the device
  repository tree.
- The workflow verifies only that the ELF is AArch64 and executable. It
  does not record `kexec --version`, the upstream tarball checksum, or a
  reproducible source commit. The actual runtime command line and loader
  mode used for EDK2 are not implemented or verified.
- The recovery UI integration currently adds an explicitly disabled
  “EDK2 Launcher (not enabled)” status page. It does not invoke kexec.

This closes the question of which kexec-tools release the successful TWRP
build generated, but does **not** establish the exact runtime loader behavior
of the installed binary or prove that it can place the current combined
BootShim+FD image at the required physical addresses.

### Next required work before any handoff code

1. Audit the ARM64 loader implementation in the exact kexec-tools 2.0.32
   source release used by CI, including image segment placement, DTB handling,
   and all relevant `kexec -l` options.
2. Add a pinned SHA-256 check for the upstream source tarball and emit
   `kexec --version`, ELF interpreter, build flags, and binary SHA-256 into
   the TWRP CI evidence.
3. Define a supported, testable way to provide the DTB and required handoff
   metadata to the BootShim; do not rely on incoming x0-x3.
4. Resolve the 2 MiB physical-placement mismatch between the 80-byte
   BootShim prefix and the fixed-linked FD at `0x90000000`.
5. Keep the WFE gate and disabled TWRP UI until these checks are complete.


## Exact kexec-tools v2.0.32 ARM64 loader audit (2026-11-13)

The TWRP CI build uses the upstream 2.0.32 release. The tagged source was
reviewed at https://github.com/horms/kexec-tools/tree/v2.0.32, specifically
`kexec/arch/arm64/kexec-arm64.c`, `kexec/arch/arm64/kexec-image-arm64.c`,
and `kexec/kexec.c`.

### Confirmed loader behavior

- The default ARM64 `Image` file type is handled by
  `image_arm64_load()`. In legacy segment mode it parses the 64-byte ARM64
  header, computes a 2 MiB-aligned hole using
  `arm64_locate_kernel_segment()`, then adds the complete input buffer as
  one segment at `kernel_segment + text_offset`, with `image_size` as the
  segment memory size.
- The ARM64 loader's `arm64_locate_kernel_segment()` calls
  `locate_hole()` with a 2 MiB alignment. Although it passes generic
  arguments `0` and `ULONG_MAX`, `locate_hole()` also clamps candidate
  ranges against the global `mem_min` and `mem_max`. The command-line
  `--mem-min` and `--mem-max` options therefore affect the legacy ARM64
  Image allocation.
- The legacy ARM64 path honors `--dtb`: it reads that file, validates and
  adjusts the FDT, and loads the resulting DTB as a separate segment after
  the kernel image. Without `--dtb`, it tries `/sys/firmware/fdt`, then
  `/proc/device-tree`. It rejects a DTB larger than 2 MiB.
- The ARM64 path also places an optional initrd after the image, then places
  the DTB and purgatory after the image. The DTB physical address is written
  into the ARM64 purgatory symbol `arm64_dtb_addr`; it is not a guarantee
  that a DTB pointer will be delivered to an arbitrary custom BootShim in
  x0.
- With `--kexec-file-syscall`, `arch_process_options()` deliberately clears
  the user-supplied `--dtb` option and the ARM64 Image loader's file-mode
  path delegates kernel parsing to the kernel syscall. The intended custom
  DTB path therefore requires the legacy `kexec_load()` segment loader, not
  the file-based syscall.
- `--mem-min` and `--mem-max` are global allocator bounds, not a fixed
  kernel-base option. A narrow upper bound that fits only the kernel image
  can prevent the later initrd/DTB/purgatory allocations, because those
  segments are also allocated through `locate_hole()`.

### Candidate aligned-prefix layout

The current 80-byte prefix cannot place the fixed-linked FD at 0x90000000
when the ARM64 hole is 2 MiB aligned and `text_offset=0x80000`. A
mathematically aligned candidate is to make the BootShim prefix exactly
`0x180000` bytes, including padding after its code:

| Item | Physical address / size |
|---|---:|
| 2 MiB-aligned kexec hole | `0x8FE00000` |
| BootShim entry (`hole + 0x80000`) | `0x8FE80000` |
| FD offset within combined Image | `0x180000` |
| FD linked base | `0x90000000` |
| FD end (2 MiB FD) | `0x90200000` |
| Combined image size | `0x380000` |
| Loader hole requirement (`text_offset + image_size`) | `0x400000` |

This arithmetic makes the FD land at its linked base only if the loader
actually selects hole `0x8FE00000`. It is not yet an approved layout:
the target recovery's live `/proc/iomem` map must show the complete required
range available to kexec, and the loader's debug output must prove the
selected `kernel_segment` equals `0x8FE00000`. The fixed destination must
also be protected from later allocations and from device DMA.

The loader selects the first eligible free range at or above `mem_min`
that satisfies the image size and alignment; `--mem-min` alone does not
prove that the resulting base equals the requested address if the target
range is unavailable. A candidate `--mem-max` must leave enough room after
the kernel image for the DTB and purgatory; do not cap it at the kernel
image end. The actual memory map and debug trace are required before
accepting any values.

### Required validation before implementation

1. Add a host-side layout test for the proposed `0x180000` prefix and
   `0x8FE00000` hole equation; keep the current 80-byte bundle unchanged
   until the test and design are reviewed.
2. Capture the target recovery's `/proc/iomem` and `/proc/device-tree`
   or `/sys/firmware/fdt` availability as read-only evidence.
3. Exercise only the load phase in a controlled test harness; capture
   `kexec -d -l` output and require the reported ARM64 `kernel_segment`
   to be exactly `0x8FE00000`. Do not execute the loaded image.
4. Verify the separate DTB and purgatory segment addresses are above the
   combined image, within usable RAM, and do not overlap the fixed FD.
5. Keep the TWRP launcher disabled and BootShim's WFE gate intact until
   physical placement, DTB transport, CPU state, DMA quiescence, and the
   EDK2 SEC entry contract are independently proven.

These findings are based on the exact upstream v2.0.32 source, not a
runtime trace from the SM-A205W. They do not authorize an on-device load,
execution, reboot, or partition write.


### Upstream kexec-tools release checksum

The kernel.org signed checksum list publishes SHA-256 for the exact release
archive used by the TWRP workflow:

- File: `kexec-tools-2.0.32.tar.xz`
- SHA-256: `8f81422a5fd2362cf6cb001b511e535565ed0f32c2f4451fb5eb68fed6710a5d`
- Checksum source: https://www.kernel.org/pub/linux/utils/kernel/kexec/sha256sums.asc

The TWRP workflow should run `sha256sum -c` immediately after downloading
the archive, then emit `kexec --version`, ELF interpreter, compiler flags,
and installed binary SHA-256 as build evidence. An attempt to update
`toufik01072002/android_device_samsung_a20/.github/workflows/twrp-a20-build.yml`
on branch `android-11` was rejected by the connected GitHub integration with
HTTP 403 (write permission unavailable); no TWRP workflow change was made.


## Placement decision and SEC entry contract (2026-11-13, after Build #38)

### Reject direct placement by a 0x180000 BootShim prefix

The earlier aligned-prefix arithmetic is correct but is not a usable placement
strategy with the current platform memory map. A \`0x180000\` prefix and
\`text_offset=0x80000\` require kexec-tools to find a contiguous
\`0x400000\` hole at \`0x8FE00000-0x90200000\`. The reference map divides this
span at \`0x90000000\`: usable RAM Bank 0 Low ends there, and the fixed FD
reservation begins there. The span is therefore not one usable RAM range.
The ordinary ARM64 Image loader adds the whole combined file as one segment;
it does not split the BootShim and FD payload into separate physical segments.
Do not use this direct-placement candidate.

### Selected layout for the next implementation step: load low, then copy FD

Keep the kexec-loadable image wholly inside ordinary usable RAM and copy the
2 MiB FD from the image payload into its fixed reserved window only after
BootShim starts:

| Item | Address / size |
|---|---:|
| ARM64 \`text_offset\` | \`0x80000\` |
| BootShim prefix (code + private stack) | \`0x10000\` |
| FD payload offset in combined Image | \`0x10000\` |
| FD payload size | \`0x200000\` |
| Combined Image \`image_size\` | \`0x210000\` |
| First-fit 2 MiB-aligned kexec hole candidate | \`0x80000000\` |
| BootShim entry candidate (\`hole + text_offset\`) | \`0x80080000\` |
| FD source candidate | \`0x80090000-0x80290000\` |
| Fixed FD destination | \`0x90000000-0x90200000\` |
| PrePi SEC entry | \`0x90008280\` |

The candidate kexec hole requires \`0x290000\` bytes from \`0x80000000\`,
which fits wholly inside the reference usable RAM range
\`0x80000000-0x90000000\`. The payload source and fixed FD destination do not
overlap. The code must remain position-independent and derive its source and
scratch-stack addresses from its runtime PC; the candidate entry address is
not to be hard-coded into the shim.

The \`0x10000\` prefix reserves \`0x8000-0x10000\` for a private 32 KiB stack.
Set \`sp\` to the aligned prefix end before branching to SEC. This is mandatory:
the actual \`EXYNOS7885Pkg/PrePi/AArch64/ModuleEntryPoint.S\` begins by calling
\`ArmPlatformPeiBootAction\`, and the platform helper
\`EXYNOS7885PkgHelper.S\` saves \`x29/x30\` to \`[sp, #-16]!\` before doing its
first operation. The kernel's incoming stack pointer must not be trusted.

### SEC entry findings from the actual platform sources

- Build #38's artifact parser identifies the fixed-linked PrePi SEC entry at
  \`0x90008280\`, with the first instruction \`BL\` targeting \`0x90008A18\`.
- The current PrePi entry does not consume the incoming Linux FDT pointer or
  require incoming \`x0-x3\`: it calls the platform boot action, computes its
  UEFI memory and stack ranges from fixed PCDs, reads MPIDR, then calls
  \`CEntryPoint(MpId, UefiMemoryBase, StacksBase)\`.
- Therefore the target kernel's clearing of \`x0-x3\` is not by itself a blocker
  for this exact SEC entry. This conclusion applies to the current built
  PrePi module, not to arbitrary future EDK2 SEC implementations.
- \`ArchPrePi.c\` sets \`HCR_EL2.TGE\` only when execution is already at EL2; it
  does not perform an EL transition. The handoff must preserve a supported
  EL and verify it from the target kexec path.
- The kexec path disables DAIF exceptions and clears the relevant SCTLR flags;
  the relocation stub executes DSB/I-cache invalidation barriers before
  branching. The BootShim copy routine must finish with explicit ordering
  and instruction-cache synchronization before entering the copied FD.
- The platform boot action accesses the DECON MMIO register and uses the
  stack before PrePi establishes its own stack. The BootShim-owned stack is
  consequently part of the entry ABI, not an optional convenience.

### Required implementation and validation gates

1. Pad the BootShim prefix to exactly \`0x10000\`, place a private stack in its
   upper \`0x8000\` bytes, and place the FD payload at offset \`0x10000\`.
2. Keep \`text_offset=0x80000\`; set \`image_size\` to the full \`0x210000\`
   combined image size.
3. Copy exactly \`0x200000\` bytes from the position-independent payload source
   to \`0x90000000\`; verify source/destination bounds and non-overlap.
4. Set a valid aligned \`sp\` before entering SEC, synchronize instruction
   fetch after the copy, and branch to \`0x90008280\` (not to the FD base).
5. Add the host-side address test \`BootShim/test_physical_layout.py\` to CI.
6. Before any real handoff, prove the actual kexec loader's selected
   \`kernel_segment\` from a load-only debug trace, prove the live memory-range
   view agrees with the reference map, and separately resolve CPU quiescence,
   DMA quiescence, and exception-level requirements.

The address test is a reference-map arithmetic proof only. It does not
execute kexec, prove the live \`/proc/iomem\` parser result, validate cache/DMA
ownership, or authorize an on-device load or execution. Keep \`handoff_enabled\`
false, the WFE gate intact, and the TWRP button disabled.
