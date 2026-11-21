# Experimental Mu-Silicium MSHC port

Status: source study / isolated port preparation. Not integrated, not built, not hardware-tested.

Base: commit e0f5cfeda003ef4c0edf1577d4ac7e9812449ea5 on port/dwmmc-samsung-idmac.

Upstream sources (Project-Silicium/Mu-Silicium, main):
- Silicon/Samsung/SamsungPkg/Drivers/MshcDxe/MshcHost.c
- Silicon/Samsung/S5E7885Pkg/Library/PlatformMshcLib/BoardInit.c

Findings:
- MshcHost.c is a UEFI DXE controller implementation and is the closest architectural reference.
- MshcInitialize() reads HCON, selects wide/narrow CLKSEL offsets, performs controller reset, calls MshcBoardPostReset(), configures FIFO/interrupts, and deliberately starts in PIO mode by clearing ENABLE_IDMAC and DMA_ENABLE.
- MshcInitDma() only enables DMA when the wide descriptor format is selected and the security unit allows it; otherwise it falls back to PIO.
- BoardInit.c contains Exynos 7885-specific CMU, GPIO, PMIC, and card-power handling. These must NOT be copied wholesale into this port: the current boot chain already owns board bring-up and duplicate clock/pin/power initialization could disturb the live controller.
- MshcBoardPostReset() sets Base+0x0B0 BIT4 and Base+0x1F8 BIT29. Those writes already exist in the reference EDK2 driver.

Port plan:
1. Extract only the controller-core sequencing and register definitions needed for a selectable experimental DXE implementation.
2. Keep the current DWMMC driver disabled for the same controller whenever this implementation is selected; never bind two host drivers to DWMMC0 or DWMMC2 concurrently.
3. Reuse existing Exynos board state and clocks; do not import Mu-Silicium PMIC/GPIO/CMU setup without separate validation.
4. Keep the experiment behind an explicit DSC/build switch. Do not alter main or the stable TWRP build.
5. Require a successful GitHub Actions EDK2 build before any user-side test.

License: upstream Mu-Silicium is BSD-2-Clause. Preserve upstream notices in any copied/ported source.
