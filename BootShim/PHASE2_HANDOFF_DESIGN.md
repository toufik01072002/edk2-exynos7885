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
