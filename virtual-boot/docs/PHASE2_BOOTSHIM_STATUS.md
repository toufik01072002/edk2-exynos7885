# Phase 2 BootShim status — SM-A205W

## Branch and safety boundary

- Working branch: `phase2/bootshim-correction`
- Base: `main`
- No changes have been merged into `main`.
- No device partitions have been written and no device-side test has been run.
- CI currently tests the BootShim against a deterministic synthetic PI firmware-volume fixture. This is not a substitute for building and validating the real EDK2 FD.

## Completed in this branch

1. Reworked the first 64 bytes as an ARM64 Linux Image header:
   - `code0`: branch to the shim body at offset 64
   - offset 4: reserved word set to zero
   - `text_offset`: 0x80000
   - `image_size`: exact size of BootShim plus appended FD
   - ARM64 magic: four exact bytes at offset 56
2. Changed the build order so the EDK2 FD is produced before the shim bundle is sized.
3. Added FD size checks: non-zero, 16-byte aligned, and no larger than the reserved 2 MiB window.
4. Added a validator for the Linux Image header and PI firmware-volume signature.
5. Added a GitHub Actions test that cross-compiles the shim and validates the bundle using a synthetic FV fixture.

## Important safety decision

The shim copies the appended FD to 0x90000000 and then stops in a WFE loop. It intentionally does not branch to 0x90000000: that address is the beginning of the firmware volume, not a proven executable SEC entry point. A green fixture test therefore means only that the wrapper/header/build mechanics pass; it does not mean UEFI boots yet.

## SEC module identified for the next stage

The platform FDF includes `EXYNOS7885Pkg/PrePi/PrePi.inf` in `FVMAIN_COMPACT`.

- SEC module GUID: `3e401783-cc94-4fcd-97bc-bd35ac369d2f`
- Module type: `SEC`
- Entry assembly: `EXYNOS7885Pkg/PrePi/AArch64/ModuleEntryPoint.S`
- Entry symbol: `_ModuleEntryPoint`
- The assembly calls `ArmPlatformPeiBootAction`, calculates the UEFI memory and stack locations from the platform PCDs, reads MPIDR, and calls `CEntryPoint`.

## Next engineering task

Build the real FD in GitHub Actions, locate the SEC FFS file by GUID, extract its TE section, parse and validate the TE entry-point metadata, and calculate the actual executable physical address using the TE image layout rules. Only after the extracted entry point and the required CPU exception-level/MMU/cache state have been verified should the shim be changed from the WFE safety stop to a SEC handoff.

The next CI stage must validate against the real generated FD, not just a synthetic FV header. It must also verify the FD destination reservation and ensure the source bundle, destination FD, kernel, initramfs, and DTB do not overlap.
