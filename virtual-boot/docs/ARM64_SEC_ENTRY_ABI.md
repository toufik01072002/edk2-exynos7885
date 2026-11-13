# ARM64 PrePi SEC entry ABI — SM-A205W

## Scope

This document records the entry conditions imposed by the **built** AArch64
PrePi SEC implementation. It is a source/build contract, not proof that the
running TWRP kernel can satisfy those conditions. The BootShim remains
fail-closed and `handoff_enabled` remains `false`.

## Actual entry chain

The SEC image is the `PrePi` module (`MODULE_TYPE = SEC`). Its TE entry is
resolved from the compiled FD by `validate_prepi_entry.py`; the resulting
address is an XIP entry inside the FD at `0x90000000`, not the FD base itself.

The entry chain is:

1. TE entry → `_ModuleEntryPoint`.
2. `_ModuleEntryPoint` calls `ArmPlatformPeiBootAction`.
3. It then derives the UEFI memory/stack placement from
   `mSystemMemoryEnd`, `PcdFdBaseAddress`, `PcdFdSize`, and
   `PcdSystemMemoryUefiRegionSize`.
4. It establishes `sp`, obtains the MPIDR, and calls `CEntryPoint` with
   `x0 = MpId`, `x1 = UefiMemoryBase`, and `x2 = StacksBase`.
5. `CEntryPoint` initializes the platform, disables the data cache,
   invalidates the instruction cache, enables the instruction cache, and
   enters the secondary-core WFE gate only when `PcdCoreCount > 1`.

## Mandatory preconditions at the first SEC instruction

- **Initial stack:** `sp` must already reference writable, mapped RAM and be
  16-byte aligned. This is mandatory because `_ModuleEntryPoint` executes
  `bl ArmPlatformPeiBootAction` *before* its own `mov sp, x1`. The stack
  calculated later by PrePi cannot satisfy this first call retroactively.
- **Executable mapping:** the resolved SEC entry and every instruction/data
  access made by the early platform action must be executable/readable under
  the active translation regime.
- **Stack mapping:** the initial stack must be writable under that same regime
  and must not overlap the FD, BootShim, kernel, initramfs, DTB, framebuffer,
  or reserved firmware memory.
- **CPU state:** the incoming exception level, security state, endianness,
  DAIF mask, and system-register state must be established from the exact
  recovery-kernel kexec path. This repository does not yet prove those values.
- **MMU/cache state:** do not assume either MMU-on or MMU-off. The SEC entry
  must be reachable with a valid mapping for its XIP address and initial
  stack. The later cache maintenance in `CEntryPoint` does not make an
  invalid initial mapping safe.
- **Registers:** do not treat the Linux kexec argument registers as the
  `CEntryPoint` arguments. The PrePi assembly creates the latter internally
  after setting its own stack and reading MPIDR.

## Current implementation decision

The current BootShim is intentionally a disabled WFE stub. It does not copy
the FD, change `sp`, or branch to SEC. No physical stack address is selected
in this change: selecting one requires a proven, non-overlapping memory map
and the exact kexec trampoline contract.

## Exit criteria before enabling a branch

1. Pin the exact recovery-kernel source and configuration used by TWRP.
2. Prove the actual kexec register/EL/MMU/cache contract from that kernel's
   final trampoline, not from generic ARM64 assumptions.
3. Select and reserve a temporary SEC-entry stack with verified bounds and
   16-byte alignment.
4. Validate the SEC entry mapping and all early accesses against that memory
   map.
5. Add an emulator/host-side test for the generated transfer stub and retain
   `handoff_enabled = false` until the hardware-specific shutdown and
   transition requirements are independently satisfied.

No flash, partition write, or device-side execution is part of this stage.
