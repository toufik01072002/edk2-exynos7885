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


## Verified recovery-kernel transition contract

The recovery-side reference is pinned to
`toufik01072002/Kernel_SM-A205W`, commit
`1e70ef871acb9ea0cd79a531b18e98f61debfc71` on the
`kexec-arm64-4.4-d-work` development line. The relevant source files are
`arch/arm64/kernel/machine_kexec.c`,
`arch/arm64/kernel/cpu-reset.S`,
`arch/arm64/kernel/cpu-reset.h`,
`arch/arm64/kernel/relocate_kernel.S`,
`arch/arm64/mm/mmu.c`, and
`arch/arm64/kernel/hyp-stub.S`.

The actual ordinary-kexec sequence in that source is:

1. `machine_kexec()` copies `arm64_relocate_new_kernel` to the physical
   control-code page and flushes its data and instruction cache lines.
2. It masks DAIF with `msr daifset, #0xf`, calls
   `setup_mm_for_reboot()` (which calls `cpu_install_idmap()`), then invokes
   `cpu_soft_restart(is_hyp_mode_available(), reboot_code_buffer_phys,
   kimage->head, kimage_start, 0)`.
3. `__cpu_soft_restart` clears `SCTLR_EL1` flags. If the kernel recorded
   EL2 boot mode, it uses `HVC #3` and the EL2 stub to call the physical
   relocation entry; otherwise it branches to that entry without the HVC
   transition.
4. `arm64_relocate_new_kernel` receives `x0 = kimage->head` and
   `x1 = kimage_start`. It clears the relevant `SCTLR_EL2` flags when
   executing at EL2, performs the physical image relocation, then performs
   `dsb nsh; ic iallu; dsb nsh; isb`.
5. Immediately before the final branch, the relocator sets
   `x0 = x1 = x2 = x3 = 0` and executes `br x18` to `kimage_start`.

### Consequences for the BootShim-to-SEC adapter

- The final image entry may be reached at **EL1 or EL2**. The branch is
  selected by `is_hyp_mode_available()`, which reflects the kernel's
  recorded boot mode. The live recovery instance's value has not been proven.
- DAIF is masked at the start of `machine_kexec()`. The transition installs
  an identity map before `cpu_soft_restart`; the ARM64 soft-restart path
  shuts down the MMU and data cache before executing the relocation code.
- The final image entry receives `x0-x3 = 0`; it must not interpret them as
  a DTB pointer or as PrePi's internal `CEntryPoint` arguments.
- **The kernel trampoline does not assign SP.** Neither
  `__cpu_soft_restart` nor `arm64_relocate_new_kernel` establishes a new
  stack before branching to the image entry. Therefore the incoming SP is
  inherited and is not a proven SEC-safe stack.
- Since PrePi calls `ArmPlatformPeiBootAction` before installing its own
  stack, the BootShim adapter must establish a dedicated writable,
  identity-accessible, 16-byte-aligned stack before branching to SEC. Its
  physical bounds must be reserved inside the actual kexec-loaded image or
  another proven safe RAM allocation; no address is selected in this change.

### TWRP launch gate

In `toufik01072002/android_bootable_recovery`, branch
`edk2-integration`, `GUIAction::edk2boot()` in `gui/action.cpp` still
returns a fail-closed error after image validation. It does not invoke
`kexec` or the kernel trampoline. This is intentional: the transition ABI
is now source-derived, but the stack placement, live EL mode, exact recovery
kernel identity/configuration, and DMA shutdown guarantees are not yet
proven.

No `kexec -e`, hardware transfer, flash, or partition write was performed.
Keep `handoff_enabled = false` and the TWRP launch action blocked.
