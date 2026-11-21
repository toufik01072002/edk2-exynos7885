#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>
#include <Library/MshcHostBridge.h>
#include "MshcHost.h"

#define MSHC_BASE                 0x13500000
#define CMU_TOP_BASE              0x12060000
#define CMU_FSYS_BASE             0x13400000
#define CLK_MUX_MMC_EMBD          (CMU_TOP_BASE + 0x1030)
#define CLK_DIV_MMC_EMBD          (CMU_TOP_BASE + 0x184C)
#define CLK_GATE_MMC_EMBD         (CMU_TOP_BASE + 0x204C)
#define PLL_MUX_MMC_EMBD          (CMU_FSYS_BASE + 0x0140)
#define GATE_MMC_EMBD_ACLK        (CMU_FSYS_BASE + 0x2038)
#define GATE_MMC_EMBD_SDCLKIN     (CMU_FSYS_BASE + 0x203C)
#define QCH_MMC_EMBD              (CMU_FSYS_BASE + 0x301C)

#define CMU_BUSY                  BIT16
#define CMU_DIV_MASK              0x1FF
#define CMU_GATE_ON               (BIT20 | BIT21)
#define CMU_USER_MUX_SEL          BIT4
#define CMU_QCH_CLOCK_REQ         BIT1
#define MSHC_CIU_CLOCK_PLL_HZ     799500000U
#define MSHC_CIU_CLOCK_MIN_HZ     25000000U
#define MSHC_CLKSEL_DIV           3
#define MSHC_EMMC_TIMING_SDR      CLKSEL_TIMING (3, 0, 4, 0)
#define MSHCI_FORCE_CLK_STOP      0x0B0
#define MSHCI_BLOCK_DMA_FOR_CI    0x1F8

STATIC EFI_STATUS
MshcWaitCmu(IN UINTN Register)
{
  for (UINTN Timeout = 10000; Timeout > 0; --Timeout) {
    if ((MmioRead32(Register) & CMU_BUSY) == 0) return EFI_SUCCESS;
    MicroSecondDelay(1);
  }
  return EFI_TIMEOUT;
}

STATIC VOID
MshcSetupEmmcClock(VOID)
{
  MmioAnd32(CLK_DIV_MMC_EMBD, ~CMU_DIV_MASK);
  MshcWaitCmu(CLK_DIV_MMC_EMBD);
  MmioAnd32(CLK_MUX_MMC_EMBD, ~1U);
  MshcWaitCmu(CLK_MUX_MMC_EMBD);
  MmioOr32(CLK_GATE_MMC_EMBD, CMU_GATE_ON);
  MmioOr32(PLL_MUX_MMC_EMBD, CMU_USER_MUX_SEL);
  MshcWaitCmu(PLL_MUX_MMC_EMBD);
  MmioOr32(GATE_MMC_EMBD_ACLK, CMU_GATE_ON);
  MmioOr32(GATE_MMC_EMBD_SDCLKIN, CMU_GATE_ON);
  MmioAndThenOr32(QCH_MMC_EMBD, ~BIT0, CMU_QCH_CLOCK_REQ);
}

UINTN
MshcBoardGetHostCount(VOID)
{
  // eMMC only: do not touch DWMMC2/microSD during this experiment.
  return 1;
}

EFI_STATUS
MshcBoardInit(IN UINTN Index, IN OUT MSHC_HOST *Host)
{
  if (Index != 0 || Host == NULL) return EFI_INVALID_PARAMETER;

  Host->ControllerId = 0;
  Host->BaseAddress = MSHC_BASE;
  Host->Coherent = FALSE;
  Host->BusWidth = 1;
  Host->ClkSelDivider = MSHC_CLKSEL_DIV;
  Host->ClkSelTiming = MSHC_EMMC_TIMING_SDR;
  Host->ClkSelTimingSdr50 = MSHC_EMMC_TIMING_SDR;
  Host->ClkSelTimingSdr104 = MSHC_EMMC_TIMING_SDR;
  Host->CiuClockHz = MSHC_CIU_CLOCK_PLL_HZ;

  // Reuse Mu-Silicium's eMMC CMU clock path; GPIO pinmux is intentionally
  // left untouched because TWRP hands off the already configured board state.
  MshcSetupEmmcClock();
  DEBUG ((EFI_D_WARN, "MSHC-A20: eMMC-only board init base=0x%lx CLKSEL=0x%08x\\n",
          (UINT64)Host->BaseAddress, Host->ClkSelTiming));
  return EFI_SUCCESS;
}

EFI_STATUS
MshcBoardSetCiuClock(IN OUT MSHC_HOST *Host, IN UINTN Frequency)
{
  UINTN Wanted, Div, Ratio;
  if (Host == NULL || Host->ControllerId != 0) return EFI_INVALID_PARAMETER;
  Wanted = (Frequency < MSHC_CIU_CLOCK_MIN_HZ) ? MSHC_CIU_CLOCK_MIN_HZ : Frequency;
  Div = Wanted * (Host->ClkSelDivider + 1);
  Ratio = (MSHC_CIU_CLOCK_PLL_HZ + Div - 1) / Div;
  if (Ratio == 0) Ratio = 1;
  if (Ratio > CMU_DIV_MASK + 1) Ratio = CMU_DIV_MASK + 1;
  MmioAndThenOr32(CLK_DIV_MMC_EMBD, ~CMU_DIV_MASK, (UINT32)(Ratio - 1));
  if (EFI_ERROR(MshcWaitCmu(CLK_DIV_MMC_EMBD))) return EFI_TIMEOUT;
  Host->CiuClockHz = MSHC_CIU_CLOCK_PLL_HZ / Ratio;
  return EFI_SUCCESS;
}

VOID
MshcBoardPostReset(IN OUT MSHC_HOST *Host)
{
  MmioOr32(Host->BaseAddress + MSHCI_FORCE_CLK_STOP, BIT4);
  MmioOr32(Host->BaseAddress + MSHCI_BLOCK_DMA_FOR_CI, BIT29);
}

EFI_STATUS
MshcBoardSetVoltage(IN OUT MSHC_HOST *Host, IN UINT32 Millivolts)
{
  (VOID)Host; (VOID)Millivolts;
  return EFI_UNSUPPORTED;
}

EFI_STATUS
MshcBoardPowerCycle(IN OUT MSHC_HOST *Host)
{
  (VOID)Host;
  return EFI_UNSUPPORTED;
}

BOOLEAN
MshcBoardIsCardPresent(IN MSHC_HOST *Host)
{
  return (Host != NULL && Host->ControllerId == 0);
}

BOOLEAN
MshcBoardIsReadOnly(IN MSHC_HOST *Host)
{
  (VOID)Host;
  return FALSE;
}
