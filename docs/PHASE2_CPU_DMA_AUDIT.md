# Phase 2 — CPU and DMA shutdown source audit

Date: 2026-11-13

## Scope and source identity

This is a source-level audit of the public Eureka Exynos7885 kernel repository
`caintlyn/Kernel-Exynos7885`, branch `R24U`, pinned at commit
`615aeae0c34ff2c4266a017d44849ca2c145da67`.

The branch is relevant to the reported
`4.4.302-p6-Eureka-R24U-KSU_R24U` kernel family, but it is **not yet proven
to be byte-for-byte the source/configuration used by the running SM-A205W**.
No runtime or device action was performed.

## Verified ordinary-kexec sequence in this source tree

1. `kernel_kexec()` in `kernel/kexec_core.c` sets
   `kexec_in_progress`, calls `kernel_restart_prepare(NULL)`, migrates to
   the reboot CPU, re-enables CPU hotplug for the kexec-specific shutdown,
   then calls `machine_shutdown()` before `machine_kexec()`.
2. `kernel_restart_prepare()` in `kernel/reboot.c` calls reboot notifiers,
   freezes userspace, disables usermode helpers, and calls
   `device_shutdown()`.
3. `machine_shutdown()` in `arch/arm64/kernel/process.c` calls
   `disable_nonboot_cpus()`. Its source comment explicitly requires all
   secondary CPUs to be fully disabled, not merely left in a RAM spin loop.

This means the normal Linux kexec core has explicit CPU-offline and
device-shutdown stages in this R24U source. The BootShim itself must not try
to park secondary CPUs in RAM as a substitute.

## DMA interpretation and limitation

The generic kexec source documents the device-shutdown stage as the mechanism
for stopping ongoing DMA and leaving devices in a state that a new kernel can
reinitialize. In this branch, the ordinary kexec path reaches that stage via
`kernel_restart_prepare() -> device_shutdown()`.

This is **not proof that every Exynos7885 device driver stops every DMA engine**.
The guarantee depends on each active driver's shutdown callback and on
platform-specific DMA engines/firmware. No device-driver-by-device-driver
audit or hardware DMA-idle measurement has been completed. Therefore DMA
quiescence remains an unresolved runtime safety gate.

## What is not yet proven for the SM-A205W

- The running recovery kernel is exactly built from this public R24U commit.
- The running kernel configuration enables `CONFIG_KEXEC` and CPU hotplug.
  The tracked A20 defconfig does not explicitly declare these symbols, so the
  final generated `.config` must be checked rather than inferred.
- The actual TWRP launch path invokes the ordinary `kernel_kexec()` path
  described above, rather than a vendor/custom transition that bypasses it.
- All active display, storage, USB, GPU, modem, and other DMA-capable devices
  complete their shutdown callbacks before control leaves Linux.
- The exact architecture-specific `machine_kexec()` implementation and
  final EL/system-register state used by the running kernel match the
  assumptions made by the EDK2 SEC entry.

## Next evidence required

Read-only checks from the exact recovery environment should capture:

- `zcat /proc/config.gz` (or the kernel's exported config) and verify
  `CONFIG_KEXEC`, `CONFIG_HOTPLUG_CPU`, and `CONFIG_SMP`.
- `uname -a`, kernel build string, and available build/version identifiers.
- The actual TWRP command and kexec-tools invocation selected for loading.
- Source/configuration identity for the running recovery kernel.

After identity is established, audit shutdown callbacks for the active
Exynos7885 DMA-capable drivers and determine whether their callbacks stop
DMA before `machine_kexec()`.

## Safety decision

No change to BootShim execution is authorized by this source audit.
`ENABLE_HANDOFF=0` remains mandatory, the packaged shim remains in its WFE
safety loop, and the TWRP EDK2 launch button remains disabled. Do not run
`kexec -e`, do not branch into the FD, and do not write boot/recovery/vbmeta
partitions based on these source findings.

## Source links

- [R24U source commit](https://github.com/caintlyn/Kernel-Exynos7885/commit/615aeae0c34ff2c4266a017d44849ca2c145da67)
- [arch/arm64/kernel/process.c](https://github.com/caintlyn/Kernel-Exynos7885/blob/615aeae0c34ff2c4266a017d44849ca2c145da67/arch/arm64/kernel/process.c)
- [kernel/kexec_core.c](https://github.com/caintlyn/Kernel-Exynos7885/blob/615aeae0c34ff2c4266a017d44849ca2c145da67/kernel/kexec_core.c)
- [kernel/reboot.c](https://github.com/caintlyn/Kernel-Exynos7885/blob/615aeae0c34ff2c4266a017d44849ca2c145da67/kernel/reboot.c)
- [A20 defconfig](https://github.com/caintlyn/Kernel-Exynos7885/blob/615aeae0c34ff2c4266a017d44849ca2c145da67/arch/arm64/configs/exynos7885-a20_defconfig)
