# A20 virtual-boot handoff ABI v1

## Scope

This ABI defines only the first transition from the running SM-A205W recovery
kernel to a second ARM64 Image entry point. It does **not** define or enable a
direct Linux-to-EDK2 jump. The second Image may later be a small BootShim that
implements a separate, documented EDK2 entry contract.

## Entry register contract

The target entry point receives the standard ARM64 Linux Image register
contract:

| Register | Value |
|---|---|
| x0 | Physical address of the unique FDT/DTB segment loaded by kexec |
| x1 | 0 |
| x2 | 0 |
| x3 | 0 |

The DTB must contain the matching Samsung A20 CAN SINGLE 05 model, the
corrected three-bank RAM map, audited reserved-memory carveouts, and the
runtime linux,initrd-start / linux,initrd-end values when an external
initramfs is supplied.

The target must reject a missing, malformed, duplicate, or incompatible DTB.
The kernel-side loader must reject a default kexec image with no unique DTB
segment rather than silently passing x0=0.

## CPU state provided by the current kexec path

The kernel path masks DAIF before entering the reset trampoline. The ARM64
soft-restart code clears the SCTLR control flags before branching, and the
relocation code performs the required data/instruction synchronization and
invalidates the instruction cache.

The existing soft-restart path chooses its EL1/EL2 transition according to
is_hyp_mode_available(). The target must inspect CurrentEL; this ABI does
not promise one fixed exception level on every bootloader/kernel state.

The kexec core disables non-boot CPUs before calling machine_kexec().
This does not prove that every DMA-capable peripheral has stopped.

## Required kernel change

The current relocate_kernel.S explicitly zeros x0-x3 before br x18. The
development patch preserves the physical DTB address in a callee-saved
register and restores it to x0 at the final branch. machine_kexec_prepare()
must locate exactly one DTB segment and pass its destination address through
the existing cpu_soft_restart() argument path.

## Explicit non-goals and blockers

- This ABI is not an EDK2 firmware ABI.
- It does not establish that the DTB RAM reservations are active in the
  currently running TWRP kernel.
- It does not prove DMA quiescence for storage, display, modem, Wi-Fi, audio,
  or other peripherals.
- It does not implement a BootShim Image header or copy the EDK2 FD.
- It does not authorize a device boot test or enable the TWRP launch button.

The only acceptance test at this stage is a successful GitHub Actions kernel
build plus static inspection of the register handoff. Device execution stays
disabled until the remaining hardware-state and memory-safety requirements
are proven.
