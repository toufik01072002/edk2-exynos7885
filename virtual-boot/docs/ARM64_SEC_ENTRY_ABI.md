# ARM64 PrePi SEC entry ABI — SM-A205W

## Scope

This document records the entry conditions imposed by the built AArch64
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
3. It then derives UEFI memory/stack placement from `mSystemMemoryEnd`,
   `PcdFdBaseAddress`, `PcdFdSize`, and `PcdSystemMemoryUefiRegionSize`.
4. It establishes `sp`, obtains the MPIDR, and calls `CEntryPoint` with
   `x0 = MpId`, `x1 = UefiMemoryBase`, and `x2 = StacksBase`.
5. `CEntryPoint` initializes the platform, disables the data cache,
   invalidates the instruction cache, enables the instruction cache, and
   enters the secondary-core WFE gate only when `PcdCoreCount > 1`.

## Mandatory preconditions at the first SEC instruction

- **Initial stack:** `sp` must already reference writable, mapped RAM and be
  16-byte aligned. This is mandatory because `_ModuleEntryPoint` executes
  `bl ArmPlatformPeiBootAction` before its own `mov sp, x1`.
- **Executable mapping:** the resolved SEC entry and every instruction/data
  access made by the early platform action must be executable/readable under
  the active translation regime.
- **Stack mapping:** the initial stack must be writable under that same regime
  and must not overlap the FD, kernel, initramfs, DTB, framebuffer, or reserved
  firmware memory.
- **CPU state:** incoming EL, security state, endianness, DAIF mask, and
  system-register state must be established from the exact recovery-kernel
  kexec path. The repository does not yet prove all live values.
- **MMU/cache state:** do not assume either MMU-on or MMU-off. SEC entry and
  the initial stack must be accessible under the actual transition state.
- **Registers:** Linux kexec argument registers are not PrePi's internal
  `CEntryPoint` arguments. PrePi creates those internally after setting SP
  and reading MPIDR.

## BootShim private stack preparation

The BootShim now reserves an 8 KiB stack inside its own 64 KiB image:

- stack bottom: image-relative offset `0xE000`;
- stack size: `0x2000` bytes (8 KiB);
- stack top: image-relative offset `0x10000`;
- FD start: bundle-relative offset `0x10000`.

The entry uses PC-relative `adr` to find the stack top, installs it in SP,
and checks 16-byte alignment before entering the disabled WFE loop. The stack
grows downward inside the BootShim reservation; its top boundary is exactly
the FD boundary, and no stack write is made into the FD. This avoids choosing
a guessed fixed physical address and gives the shim a deterministic,
position-independent stack location.

This is a static layout and instruction check only. It does not prove that
the live kexec loader places the complete BootShim image in writable,
non-overlapping RAM or that the image mapping is suitable at runtime. The
BootShim still does not copy the FD, calculate or branch to the SEC entry, or
enable handoff.

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

The ordinary-kexec path in that source:

1. Copies `arm64_relocate_new_kernel` to the physical control-code page and
   flushes its data and instruction cache lines.
2. Masks DAIF, calls `setup_mm_for_reboot()` (which calls
   `cpu_install_idmap()`), then invokes `cpu_soft_restart()`.
3. The soft-restart path clears SCTLR flags and selects the EL1/EL2 transition
   according to the kernel's recorded boot mode.
4. The relocator performs physical image relocation and cache maintenance.
5. Immediately before the final branch, it sets `x0-x3 = 0` and executes
   `br x18` to `kimage_start`.

Consequences:

- The final image entry may be reached at EL1 or EL2; the live mode for this
  recovery instance is not yet proven.
- The final image entry receives `x0-x3 = 0`; these are not a DTB pointer.
- The kernel trampoline does not assign SP. Incoming SP is inherited and is
  not a proven SEC-safe stack.
- The BootShim's private stack setup addresses this inherited-SP problem at
  the static image level, but actual RAM placement, writability, and
  non-overlap still require runtime evidence.

## Exit criteria before enabling a branch

1. Prove the exact recovery-kernel source, configuration, and live kexec path.
2. Prove live EL, MMU/cache, security-state, interrupt, and system-register
   conditions at the BootShim entry.
3. Prove the loaded BootShim's physical range is writable, reserved, and
   non-overlapping with every other virtual-boot component.
4. Validate the SEC XIP entry mapping and all early accesses against that map.
5. Implement and test the FD-to-SEC transfer, including all required cache,
   memory, and secondary-core handling.
6. Keep `handoff_enabled = false` and the TWRP launch action blocked until
   the complete hardware-specific transition is independently validated.

No flash, partition write, kexec execution, or device-side execution is part
of this stage.
