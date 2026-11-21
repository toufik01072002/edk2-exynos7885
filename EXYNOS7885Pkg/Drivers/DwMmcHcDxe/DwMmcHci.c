#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include "DwMmcHci.h"

#define DW_MMC_MAX_DESC_COUNT      256
#define DW_MMC_MAX_BYTES_PER_DESC  4096
#define DW_MMC_BOUNCE_BUF_SIZE     (DW_MMC_MAX_DESC_COUNT * DW_MMC_MAX_BYTES_PER_DESC)
#define DW_MMC_DMA_MAX_ADDR        0x87FFFFFFULL

#ifndef EXYNOS_DWMMC_UHS_REG_EXT
#define EXYNOS_DWMMC_UHS_REG_EXT   0x108
#endif
#ifndef EXYNOS_DWMMC_DDR_REG
#define EXYNOS_DWMMC_DDR_REG       0x10C
#endif

// Masque des bits HS400 / DDR dans UHS_REG_EXT (0x108) sur Exynos 7885
#define EXYNOS_UHS_EXT_HS400_MASK       (BIT31 | BIT30 | BIT29 | BIT28 | BIT27 | BIT26 | BIT25 | BIT24)

// Samsung CLKSEL64 timing fields from the SM-A205W kernel driver.
#define EXYNOS_DWMMC_CLKSEL_SAMPLE_MASK  0x00000007U
#define EXYNOS_DWMMC_CLKSEL_TIMING_MASK  ((0x7U << 24) | (0x3U << 22) | \
                                          (0x7U << 16) | 0x7U)
#define EXYNOS_DWMMC0_SDR_TIMING         ((3U << 24) | (0U << 22) | \
                                          (4U << 16) | 0U)

STATIC UINT32 mSavedClkSel0 = 0x03040000;
STATIC UINT32 mSavedClkSel2 = 0x03040000;
STATIC BOOLEAN mRxTuned0    = FALSE;
STATIC BOOLEAN mRxTuned2    = FALSE;

STATIC DW_MMC_IDMAC_DESC64 *mDescRing0  = NULL;
STATIC DW_MMC_IDMAC_DESC64 *mDescRing2  = NULL;
STATIC VOID                *mBounceBuf0 = NULL;
STATIC VOID                *mBounceBuf2 = NULL;

STATIC
EFI_STATUS
DwMmcWaitForClear (
  IN UINTN  BaseAddr,
  IN UINT32 Offset,
  IN UINT32 Mask,
  IN UINT32 Timeout
  )
{
  while (Timeout > 0) {
    if ((MmioRead32 (BaseAddr + Offset) & Mask) == 0) {
      return EFI_SUCCESS;
    }
    MicroSecondDelay (1);
    Timeout--;
  }

  return ((MmioRead32 (BaseAddr + Offset) & Mask) == 0)
         ? EFI_SUCCESS : EFI_TIMEOUT;
}

STATIC
UINT32
DwMmcGetSavedClkSel (
  IN UINTN BaseAddr
  )
{
  return (BaseAddr == EXYNOS_DWMMC0_BASE) ? mSavedClkSel0 : mSavedClkSel2;
}

STATIC
VOID
DwMmcSetSavedClkSel (
  IN UINTN  BaseAddr,
  IN UINT32 Val
  )
{
  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    mSavedClkSel0 = Val;
  } else {
    mSavedClkSel2 = Val;
  }
}

STATIC
VOID
DwMmcCleanExynosSbootRegs (
  IN UINTN BaseAddr
  )
{
  UINT32 UhsExt;

  // Desactiver tout residu DDR / HS400 configure par S-Boot
  MmioWrite32 (BaseAddr + DW_MMC_UHS_REG, 0);
  MmioWrite32 (BaseAddr + EXYNOS_DWMMC_DDR_REG, 0);

  UhsExt = MmioRead32 (BaseAddr + EXYNOS_DWMMC_UHS_REG_EXT);
  UhsExt &= ~EXYNOS_UHS_EXT_HS400_MASK;
  MmioWrite32 (BaseAddr + EXYNOS_DWMMC_UHS_REG_EXT, UhsExt);
}

STATIC
EFI_STATUS
DwMmcEnsureDmaBuffers (
  IN  UINTN                BaseAddr,
  OUT DW_MMC_IDMAC_DESC64  **OutRing,
  OUT VOID                 **OutBounce
  )
{
  EFI_STATUS           Status;
  EFI_PHYSICAL_ADDRESS PhysAddr;
  DW_MMC_IDMAC_DESC64  **RingPtr;
  VOID                 **BouncePtr;
  UINTN                DescPages;
  UINTN                BouncePages;

  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    RingPtr   = &mDescRing0;
    BouncePtr = &mBounceBuf0;
  } else {
    RingPtr   = &mDescRing2;
    BouncePtr = &mBounceBuf2;
  }

  if (*RingPtr == NULL) {
    DescPages = EFI_SIZE_TO_PAGES (DW_MMC_MAX_DESC_COUNT * sizeof (DW_MMC_IDMAC_DESC64));
    PhysAddr  = DW_MMC_DMA_MAX_ADDR;
    Status = gBS->AllocatePages (
                    AllocateMaxAddress,
                    EfiBootServicesData,
                    DescPages,
                    &PhysAddr
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }
    *RingPtr = (DW_MMC_IDMAC_DESC64 *)(UINTN)PhysAddr;
    ZeroMem (*RingPtr, EFI_PAGES_TO_SIZE (DescPages));
    WriteBackInvalidateDataCacheRange (*RingPtr, EFI_PAGES_TO_SIZE (DescPages));
  }

  if (*BouncePtr == NULL) {
    BouncePages = EFI_SIZE_TO_PAGES (DW_MMC_BOUNCE_BUF_SIZE);
    PhysAddr    = DW_MMC_DMA_MAX_ADDR;
    Status = gBS->AllocatePages (
                    AllocateMaxAddress,
                    EfiBootServicesData,
                    BouncePages,
                    &PhysAddr
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }
    *BouncePtr = (VOID *)(UINTN)PhysAddr;
    ZeroMem (*BouncePtr, DW_MMC_BOUNCE_BUF_SIZE);
    WriteBackInvalidateDataCacheRange (*BouncePtr, DW_MMC_BOUNCE_BUF_SIZE);
  }

  *OutRing   = *RingPtr;
  *OutBounce = *BouncePtr;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
DwMmcEnableIdmac (
  IN UINTN BaseAddr
  )
{
  EFI_STATUS Status;

  MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_SWR);
  Status = DwMmcWaitForClear (BaseAddr, DW_MMC_BMOD, DW_MMC_BMOD_SWR, 10000);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
  // Match Samsung Linux dw_mci_idmac_init(): normal, Rx and Tx completion.
  MmioWrite32 (
    BaseAddr + DW_MMC_IDINTEN,
    DW_MMC_IDMAC_INT_NI | DW_MMC_IDMAC_INT_RI | DW_MMC_IDMAC_INT_TI
    );

  // Do not enable the DMA engine here. Linux enables CTRL.USE_IDMAC,
  // BMOD.DE and BMOD.FB for each data request after DBADDR is programmed.
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
DwMmcRecoverController (
  IN UINTN  BaseAddr,
  IN UINT32 CmdIdx
  )
{
  EFI_STATUS Status;
  UINT32     Timeout;
  UINT32     RintSts;
  UINT32     Ctrl;
  UINT32     ClkSelSaved;

  ClkSelSaved = DwMmcGetSavedClkSel (BaseAddr);

  MmioWrite32 (
    BaseAddr + EXYNOS_DWMMC_CLKSEL,
    ClkSelSaved & ~((0x3 << 6) | 0x7)
    );

  Ctrl = MmioRead32 (BaseAddr + DW_MMC_CTRL);
  Ctrl |= DW_MMC_CTRL_RESET | DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET;
  MmioWrite32 (BaseAddr + DW_MMC_CTRL, Ctrl);

  Status = DwMmcWaitForClear (
             BaseAddr,
             DW_MMC_CTRL,
             DW_MMC_CTRL_RESET | DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET,
             100000
             );

  MmioWrite32 (BaseAddr + EXYNOS_DWMMC_CLKSEL, ClkSelSaved);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  DwMmcCleanExynosSbootRegs (BaseAddr);
  Status = DwMmcEnableIdmac (BaseAddr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_CMDARG, 0);

  MmioWrite32 (
    BaseAddr + DW_MMC_CMD,
    DW_MMC_CMD_START | DW_MMC_CMD_UPD_CLK | BIT29
    );

  Timeout = 100000;
  while (Timeout > 0) {
    RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);

    if (RintSts & DW_MMC_INT_HLE) {
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, DW_MMC_INT_HLE);
      return EFI_DEVICE_ERROR;
    }

    if ((MmioRead32 (BaseAddr + DW_MMC_CMD) & DW_MMC_CMD_START) == 0) {
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
    Timeout--;
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
DwMmcUpdateClock (
  IN UINTN BaseAddr
  )
{
  EFI_STATUS Status;
  UINT32     Timeout;
  UINT32     CmdVal;
  UINT32     RintSts;

  Status = DwMmcWaitForClear (
             BaseAddr,
             DW_MMC_CMD,
             DW_MMC_CMD_START,
             100000
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_CMDARG, 0);

  CmdVal = DW_MMC_CMD_START |
           DW_MMC_CMD_UPD_CLK |
           DW_MMC_CMD_WAIT_PRVDATA |
           BIT29;

  MmioWrite32 (BaseAddr + DW_MMC_CMD, CmdVal);

  Timeout = 100000;
  while (Timeout > 0) {
    RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);

    if (RintSts & DW_MMC_INT_HLE) {
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, DW_MMC_INT_HLE);
      return EFI_DEVICE_ERROR;
    }

    if ((MmioRead32 (BaseAddr + DW_MMC_CMD) & DW_MMC_CMD_START) == 0) {
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
    Timeout--;
  }

  return EFI_TIMEOUT;
}

EFI_STATUS
DwMmcInitController (
  IN UINTN BaseAddr
  )
{
  UINT32     ClkSelInit;
  UINT32     Ctrl;
  UINT32     UhsExtBefore;
  UINT32     DdrBefore;
  EFI_STATUS Status;

  UhsExtBefore = MmioRead32 (BaseAddr + EXYNOS_DWMMC_UHS_REG_EXT);
  DdrBefore    = MmioRead32 (BaseAddr + EXYNOS_DWMMC_DDR_REG);

  ClkSelInit = MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL);
  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    // SM-A205W eMMC DTS: <3 0 4 0> = divider, fine-drive, drive, sample.
    ClkSelInit = (ClkSelInit & ~EXYNOS_DWMMC_CLKSEL_TIMING_MASK) |
                 EXYNOS_DWMMC0_SDR_TIMING;
    ClkSelInit &= ~(BIT30 | BIT19);
  } else if (ClkSelInit == 0) {
    ClkSelInit = 0x03040000;
  }
  DwMmcSetSavedClkSel (BaseAddr, ClkSelInit);

  MmioWrite32 (BaseAddr + DW_MMC_BMOD, 0);
  MmioWrite32 (BaseAddr + DW_MMC_IDINTEN, 0);

  Ctrl = MmioRead32 (BaseAddr + DW_MMC_CTRL);
  Ctrl &= ~(DW_MMC_CTRL_USE_IDMAC |
            DW_MMC_CTRL_DMA_ENABLE |
            DW_MMC_CTRL_INT_ENABLE);
  Ctrl |= DW_MMC_CTRL_RESET |
          DW_MMC_CTRL_FIFO_RESET |
          DW_MMC_CTRL_DMA_RESET;

  MmioWrite32 (BaseAddr + DW_MMC_CTRL, Ctrl);

  Status = DwMmcWaitForClear (
             BaseAddr,
             DW_MMC_CTRL,
             DW_MMC_CTRL_RESET |
             DW_MMC_CTRL_FIFO_RESET |
             DW_MMC_CTRL_DMA_RESET,
             100000
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Nettoyer les registres DDR / HS400 Samsung laisses actifs par S-Boot
  DwMmcCleanExynosSbootRegs (BaseAddr);

  MmioWrite32 (BaseAddr + EXYNOS_DWMMC_CLKSEL, ClkSelInit);
  MmioWrite32 (BaseAddr + DW_MMC_PWREN, 1);
  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_INTMASK, 0);
  MmioWrite32 (BaseAddr + DW_MMC_TMOUT, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_CTYPE, DW_MMC_CTYPE_1BIT);
  MmioWrite32 (BaseAddr + DW_MMC_FIFOTH,
               (0x2 << 28) | (15 << 16) | 16);

  Status = DwMmcEnableIdmac (BaseAddr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    Print (L"[DW0] VERID=0x%x HCON=0x%x CLKSEL=0x%x UHS_EXT=0x%x->0x%x DDR=0x%x->0x%x\n",
           MmioRead32 (BaseAddr + DW_MMC_VERID),
           MmioRead32 (BaseAddr + DW_MMC_HCON),
           MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL),
           UhsExtBefore, MmioRead32 (BaseAddr + EXYNOS_DWMMC_UHS_REG_EXT),
           DdrBefore, MmioRead32 (BaseAddr + EXYNOS_DWMMC_DDR_REG));
  }

  return DwMmcSetClock (BaseAddr, 400);
}

EFI_STATUS
DwMmcSetClock (
  IN UINTN  BaseAddr,
  IN UINT32 ClockFreqKhz
  )
{
  UINT32     Div;
  EFI_STATUS Status;

  MmioWrite32 (BaseAddr + DW_MMC_CLKENA, 0);

  Status = DwMmcUpdateClock (BaseAddr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (ClockFreqKhz == 0) {
    return EFI_SUCCESS;
  }

  if (ClockFreqKhz <= 400) {
    Div = 64;
  } else {
    Div = 16;
  }

  MmioWrite32 (BaseAddr + DW_MMC_CLKSRC, 0);
  MmioWrite32 (BaseAddr + DW_MMC_CLKDIV, Div);

  Status = DwMmcUpdateClock (BaseAddr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  MmioWrite32 (BaseAddr + DW_MMC_CLKENA, DW_MMC_CLKENA_CCLK_ENABLE);

  Status = DwMmcUpdateClock (BaseAddr);
  if (EFI_ERROR (Status)) {
    MmioWrite32 (BaseAddr + DW_MMC_CLKENA, 0);
    return Status;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
DwMmcSetBusWidth (
  IN UINTN BaseAddr,
  IN UINT8 BusWidth
  )
{
  UINT32 Ctype = DW_MMC_CTYPE_1BIT;

  if (BusWidth == 4) {
    Ctype = DW_MMC_CTYPE_4BIT;
  } else if (BusWidth == 8) {
    Ctype = DW_MMC_CTYPE_8BIT;
  }

  MmioWrite32 (BaseAddr + DW_MMC_CTYPE, Ctype);
  return DwMmcUpdateClock (BaseAddr);
}

STATIC
EFI_STATUS
DwMmcSetupIdmacDescriptors (
  IN UINTN                BaseAddr,
  IN DW_MMC_IDMAC_DESC64  *Ring,
  IN VOID                 *BounceBuf,
  IN UINT32               TotalBytes
  )
{
  UINT32               Remaining;
  UINT32               DescCount;
  UINT32               i;
  UINT32               Chunk;
  EFI_PHYSICAL_ADDRESS BufAddr;
  EFI_PHYSICAL_ADDRESS RingAddr;
  EFI_PHYSICAL_ADDRESS NextAddr;
  UINT32               Des0;

  if (TotalBytes > DW_MMC_BOUNCE_BUF_SIZE) {
    return EFI_BAD_BUFFER_SIZE;
  }

  DescCount = (TotalBytes + DW_MMC_MAX_BYTES_PER_DESC - 1) / DW_MMC_MAX_BYTES_PER_DESC;
  if (DescCount > DW_MMC_MAX_DESC_COUNT) {
    return EFI_BAD_BUFFER_SIZE;
  }

  RingAddr  = (EFI_PHYSICAL_ADDRESS)(UINTN)Ring;
  BufAddr   = (EFI_PHYSICAL_ADDRESS)(UINTN)BounceBuf;
  Remaining = TotalBytes;

  for (i = 0; i < DescCount; i++) {
    Chunk = (Remaining > DW_MMC_MAX_BYTES_PER_DESC) ? DW_MMC_MAX_BYTES_PER_DESC : Remaining;

    // Linux idmac_desc_64addr: OWN | DIC | CH, FD on first, LD on last.
    Des0 = IDMAC_DES0_OWN | IDMAC_DES0_DIC | IDMAC_DES0_CH;
    if (i == 0) {
      Des0 |= IDMAC_DES0_FD; // First descriptor (FD)
    }
    if (i == (DescCount - 1)) {
      Des0 &= ~(IDMAC_DES0_CH | IDMAC_DES0_DIC);
      Des0 |= IDMAC_DES0_LD;
      NextAddr = 0;
    } else {
      NextAddr = RingAddr + ((i + 1) * sizeof (DW_MMC_IDMAC_DESC64));
    }

    Ring[i].Des0           = Des0;
    Ring[i].Des1           = 0;
    Ring[i].Des2           = IDMAC_SET_BUFFER1_SIZE (Chunk);
    Ring[i].Des3           = 0;
    Ring[i].Buffer1AddrLo  = (UINT32)(BufAddr & 0xFFFFFFFFULL);
    Ring[i].Buffer1AddrHi  = (UINT32)(BufAddr >> 32);
    Ring[i].NextDescAddrLo = (UINT32)(NextAddr & 0xFFFFFFFFULL);
    Ring[i].NextDescAddrHi = (UINT32)(NextAddr >> 32);

    BufAddr   += Chunk;
    Remaining -= Chunk;
  }

  WriteBackInvalidateDataCacheRange (Ring, DescCount * sizeof (DW_MMC_IDMAC_DESC64));

  // Linux dw_mci_idmac_init() resets BMOD once during controller setup.
  // Keep this per-request path limited to publishing the descriptor base.
  MmioWrite32 (BaseAddr + DW_MMC_DBADDRL, (UINT32)(RingAddr & 0xFFFFFFFFULL));
  MmioWrite32 (BaseAddr + DW_MMC_DBADDRU, (UINT32)(RingAddr >> 32));
  MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
DwMmcExecuteSingleTransaction (
  IN     UINTN                               BaseAddr,
  IN OUT EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *Packet,
  OUT    UINT32                              *OutRintSts
  )
{
  UINT32              CmdReg = DW_MMC_CMD_START | BIT29;
  UINT32              RintSts = 0;
  UINT32              IdSts   = 0;
  UINT32              Timeout;
  EFI_STATUS          Status;
  BOOLEAN             IsRead     = FALSE;
  UINT32              TotalBytes = 0;
  VOID                *UserBuf   = NULL;
  UINT16              CmdIdx;
  UINT32              CmdArg;
  BOOLEAN             IsDeselect;
  DW_MMC_IDMAC_DESC64 *Ring      = NULL;
  VOID                *BounceBuf = NULL;

  if (OutRintSts != NULL) {
    *OutRintSts = 0;
  }

  CmdIdx     = Packet->SdMmcCmdBlk->CommandIndex;
  CmdArg     = Packet->SdMmcCmdBlk->CommandArgument;
  IsDeselect = (CmdIdx == 7 && CmdArg == 0);

  if (CmdIdx != 13 && CmdIdx != 12 && !IsDeselect) {
    CmdReg |= DW_MMC_CMD_WAIT_PRVDATA;
  }

  Status = DwMmcWaitForClear (BaseAddr, DW_MMC_CMD, DW_MMC_CMD_START, 200000);
  if (EFI_ERROR (Status)) {
    DwMmcRecoverController (BaseAddr, CmdIdx);
    return EFI_TIMEOUT;
  }

  if (CmdIdx != 13 && CmdIdx != 12 && !IsDeselect) {
    Status = DwMmcWaitForClear (BaseAddr, DW_MMC_STATUS, DW_MMC_STATUS_DATA_BUSY, 200000);
    if (EFI_ERROR (Status)) {
      return EFI_TIMEOUT;
    }
  }

  if (CmdIdx == 7 || CmdIdx == 8 || CmdIdx == 17 || CmdIdx == 18) {
    if (MmioRead32 (BaseAddr + DW_MMC_CLKDIV) == 64) {
      Status = DwMmcSetClock (BaseAddr, 25000);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);

  CmdReg |= (CmdIdx & 0x3F);

  if (CmdIdx == 0) {
    CmdReg |= DW_MMC_CMD_SEND_INIT;
  }

  if (!IsDeselect) {
    switch (Packet->SdMmcCmdBlk->ResponseType) {
      case SdMmcResponseTypeR1:
      case SdMmcResponseTypeR1b:
      case SdMmcResponseTypeR6:
      case SdMmcResponseTypeR7:
        CmdReg |= (DW_MMC_CMD_RESP_EXP | DW_MMC_CMD_CHECK_RESP_CRC);
        break;
      case SdMmcResponseTypeR2:
        CmdReg |= (DW_MMC_CMD_RESP_EXP | DW_MMC_CMD_RESP_LENGTH | DW_MMC_CMD_CHECK_RESP_CRC);
        break;
      case SdMmcResponseTypeR3:
      case SdMmcResponseTypeR4:
        CmdReg |= DW_MMC_CMD_RESP_EXP;
        break;
      default:
        break;
    }
  }

  if (Packet->InDataBuffer != NULL && Packet->InTransferLength > 0) {
    IsRead     = TRUE;
    TotalBytes = Packet->InTransferLength;
    UserBuf    = Packet->InDataBuffer;
    CmdReg    |= DW_MMC_CMD_DATA_EXP;
  } else if (Packet->OutDataBuffer != NULL && Packet->OutTransferLength > 0) {
    IsRead     = FALSE;
    TotalBytes = Packet->OutTransferLength;
    UserBuf    = Packet->OutDataBuffer;
    CmdReg    |= (DW_MMC_CMD_DATA_EXP | DW_MMC_CMD_RW);
  }

  if (TotalBytes > 0 && UserBuf != NULL) {
    Status = DwMmcEnsureDmaBuffers (BaseAddr, &Ring, &BounceBuf);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (IsRead) {
      ZeroMem (BounceBuf, TotalBytes);
    } else {
      CopyMem (BounceBuf, UserBuf, TotalBytes);
    }
    WriteBackInvalidateDataCacheRange (BounceBuf, TotalBytes);

    MmioOr32 (BaseAddr + DW_MMC_CTRL, DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET);
    Status = DwMmcWaitForClear (
               BaseAddr,
               DW_MMC_CTRL,
               DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET,
               100000
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    // Match dw_mci_idmac_start_dma(): select IDMAC, then enable BMOD.
    // CTRL.DMA_ENABLE is not set by the Samsung Linux IDMAC path.
    MmioOr32 (BaseAddr + DW_MMC_CTRL, DW_MMC_CTRL_USE_IDMAC);

    MmioWrite32 (BaseAddr + DW_MMC_BLKSIZ, (TotalBytes < 512) ? TotalBytes : 512);
    MmioWrite32 (BaseAddr + DW_MMC_BYTCNT, TotalBytes);

    Status = DwMmcSetupIdmacDescriptors (BaseAddr, Ring, BounceBuf, TotalBytes);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (CmdIdx == 18 || CmdIdx == 25) {
      CmdReg |= DW_MMC_CMD_SEND_STOP;
    }
  }

  ArmDataSynchronizationBarrier ();

  // Match Samsung Linux dw_mci_idmac_start_dma(): preserve BMOD state,
  // enable descriptor DMA, then issue the poll demand before the command.
  if (TotalBytes > 0) {
    UINT32 Bmod = MmioRead32 (BaseAddr + DW_MMC_BMOD);
    Bmod |= DW_MMC_BMOD_DE | DW_MMC_BMOD_FB;
    MmioWrite32 (BaseAddr + DW_MMC_BMOD, Bmod);
    ArmDataSynchronizationBarrier ();
    MmioWrite32 (BaseAddr + DW_MMC_PLDMND, 1);
    ArmDataSynchronizationBarrier ();
  }

  MmioWrite32 (BaseAddr + DW_MMC_CMDARG, CmdArg);
  MmioWrite32 (BaseAddr + DW_MMC_CMD, CmdReg);

  Timeout = 200000;
  while (Timeout > 0) {
    RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
    if (RintSts & DW_MMC_INT_CMD_DONE) {
      break;
    }
    if (RintSts & (DW_MMC_INT_RE | DW_MMC_INT_RCRC | DW_MMC_INT_RTO | DW_MMC_INT_HLE)) {
      if (OutRintSts != NULL) {
        *OutRintSts = RintSts;
      }
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
      return EFI_DEVICE_ERROR;
    }
    MicroSecondDelay (1);
    Timeout--;
  }

  if ((MmioRead32 (BaseAddr + DW_MMC_RINTSTS) & DW_MMC_INT_CMD_DONE) == 0) {
    DwMmcRecoverController (BaseAddr, CmdIdx);
    return EFI_TIMEOUT;
  }

  if (Packet->SdMmcStatusBlk != NULL && !IsDeselect) {
    if (Packet->SdMmcCmdBlk->ResponseType == SdMmcResponseTypeR2) {
      Packet->SdMmcStatusBlk->Resp0 = MmioRead32 (BaseAddr + DW_MMC_RESP0);
      Packet->SdMmcStatusBlk->Resp1 = MmioRead32 (BaseAddr + DW_MMC_RESP1);
      Packet->SdMmcStatusBlk->Resp2 = MmioRead32 (BaseAddr + DW_MMC_RESP2);
      Packet->SdMmcStatusBlk->Resp3 = MmioRead32 (BaseAddr + DW_MMC_RESP3);
    } else if (Packet->SdMmcCmdBlk->ResponseType >= SdMmcResponseTypeR1 &&
               Packet->SdMmcCmdBlk->ResponseType <= SdMmcResponseTypeR7) {
      Packet->SdMmcStatusBlk->Resp0 = MmioRead32 (BaseAddr + DW_MMC_RESP0);
    }
  }

  if (TotalBytes > 0 && UserBuf != NULL) {
    UINT32 DmaDoneMask;

    DmaDoneMask = IsRead ? DW_MMC_IDMAC_INT_RI : DW_MMC_IDMAC_INT_TI;
    Timeout = 500000;
    while (Timeout > 0) {
      RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
      IdSts   = MmioRead32 (BaseAddr + DW_MMC_IDSTS);

      if (OutRintSts != NULL) {
        *OutRintSts = RintSts;
      }

      if (IdSts & DW_MMC_IDMAC_INT_ERR) {
        Print (L"[DWMMC DMA-ERR] CMD=%u IDSTS=0x%x RINT=0x%x CTRL=0x%x BMOD=0x%x DBADDR=%08x:%08x DESC0=0x%x DESC2=0x%x BUF=%08x:%08x NEXT=%08x:%08x\n",
               CmdIdx, IdSts, RintSts,
               MmioRead32 (BaseAddr + DW_MMC_CTRL),
               MmioRead32 (BaseAddr + DW_MMC_BMOD),
               MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL),
               MmioRead32 (BaseAddr + DW_MMC_CLKDIV),
               MmioRead32 (BaseAddr + DW_MMC_DBADDRU),
               MmioRead32 (BaseAddr + DW_MMC_DBADDRL),
               Ring[0].Des0, Ring[0].Des2,
               Ring[0].Buffer1AddrHi, Ring[0].Buffer1AddrLo,
               Ring[0].NextDescAddrHi, Ring[0].NextDescAddrLo);
        MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
        MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
        return EFI_DEVICE_ERROR;
      }

      if (RintSts & DW_MMC_INT_ERROR) {
        Print (L"[DWMMC DATA-ERR] CMD=%u IDSTS=0x%x RINT=0x%x CTRL=0x%x BMOD=0x%x DBADDR=%08x:%08x DESC0=0x%x DESC2=0x%x\n",
               CmdIdx, IdSts, RintSts,
               MmioRead32 (BaseAddr + DW_MMC_CTRL),
               MmioRead32 (BaseAddr + DW_MMC_BMOD),
               MmioRead32 (BaseAddr + DW_MMC_DBADDRU),
               MmioRead32 (BaseAddr + DW_MMC_DBADDRL),
               Ring[0].Des0, Ring[0].Des2);
        MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
        MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
        return EFI_CRC_ERROR;
      }

      // DATA_OVER and IDMAC RI/TI are independent completion indications.
      if ((RintSts & DW_MMC_INT_DTO) && (IdSts & DmaDoneMask)) {
        break;
      }

      MicroSecondDelay (1);
      Timeout--;
    }

    if (((RintSts & DW_MMC_INT_DTO) == 0) || ((IdSts & DmaDoneMask) == 0)) {
      Print (L"[DWMMC DMA-TIMEOUT] CMD=%u READ=%u IDSTS=0x%x RINT=0x%x CTRL=0x%x BMOD=0x%x DBADDR=%08x:%08x DESC0=0x%x DESC2=0x%x BUF=%08x:%08x NEXT=%08x:%08x\n",
             CmdIdx, IsRead, IdSts, RintSts,
             MmioRead32 (BaseAddr + DW_MMC_CTRL),
             MmioRead32 (BaseAddr + DW_MMC_BMOD),
             MmioRead32 (BaseAddr + DW_MMC_DBADDRU),
             MmioRead32 (BaseAddr + DW_MMC_DBADDRL),
             Ring[0].Des0, Ring[0].Des2,
             Ring[0].Buffer1AddrHi, Ring[0].Buffer1AddrLo,
             Ring[0].NextDescAddrHi, Ring[0].NextDescAddrLo);
      MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
      return EFI_TIMEOUT;
    }

    if (IsRead) {
      InvalidateDataCacheRange (BounceBuf, TotalBytes);
      CopyMem (UserBuf, BounceBuf, TotalBytes);
    }

    MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
    MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  }

  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  return EFI_SUCCESS;
}

EFI_STATUS
DwMmcExecCommand (
  IN     UINTN                               BaseAddr,
  IN OUT EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *Packet
  )
{
  EFI_STATUS Status;
  UINT32     RintSts = 0;
  UINT16     CmdIdx;
  UINT32     CmdArg;
  BOOLEAN    *TunedFlag;
  UINT32     Phase;
  UINT32     BaseClkSel;

  if (Packet == NULL || Packet->SdMmcCmdBlk == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  CmdIdx = Packet->SdMmcCmdBlk->CommandIndex;
  CmdArg = Packet->SdMmcCmdBlk->CommandArgument;

  // Autoriser CMD6 sur DWMMC0 pour synchroniser la largeur de bus (CTYPE) avec l'eMMC
  Status = DwMmcExecuteSingleTransaction (BaseAddr, Packet, &RintSts);

  if (!EFI_ERROR (Status) && CmdIdx == 6) {
    UINT8 Access = (CmdArg >> 24) & 0x3;
    UINT8 Index  = (CmdArg >> 16) & 0xFF;
    UINT8 Value  = (CmdArg >> 8) & 0xFF;

    // SWITCH Write Byte (Access == 3) sur EXT_CSD[183] (BUS_WIDTH)
    if (Access == 3 && Index == 183) {
      if (Value == 0) {
        DwMmcSetBusWidth (BaseAddr, 1);
      } else if (Value == 1 || Value == 5) {
        DwMmcSetBusWidth (BaseAddr, 4);
      } else if (Value == 2 || Value == 6) {
        DwMmcSetBusWidth (BaseAddr, 8);
      }
    }
  }

  TunedFlag = (BaseAddr == EXYNOS_DWMMC0_BASE) ? &mRxTuned0 : &mRxTuned2;
  if (Status == EFI_CRC_ERROR && Packet->InDataBuffer != NULL && !(*TunedFlag)) {
    EFI_STATUS ClockStatus;

    *TunedFlag = TRUE;
    BaseClkSel = DwMmcGetSavedClkSel (BaseAddr) & ~EXYNOS_DWMMC_CLKSEL_SAMPLE_MASK;

    // Phase 0 was tested by the initial transaction. Preserve Samsung's
    // divider/drive fields while testing the remaining RX sample phases.
    for (Phase = 1; Phase < 8; Phase++) {
      UINT32 TestClkSel = BaseClkSel | Phase;

      MmioWrite32 (BaseAddr + EXYNOS_DWMMC_CLKSEL, TestClkSel);
      ArmDataSynchronizationBarrier ();
      ClockStatus = DwMmcUpdateClock (BaseAddr);
      if (EFI_ERROR (ClockStatus)) {
        Print (L"[DWMMC CLK-ERR] CMD=%u phase=%u CLKSEL=0x%x status=0x%lx\n",
               CmdIdx, Phase, TestClkSel, (UINT64)ClockStatus);
        Status = ClockStatus;
        break;
      }

      Print (L"[DWMMC CLK] CMD=%u phase=%u CLKSEL=0x%x CLKDIV=%u\n",
             CmdIdx, Phase, MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL),
             MmioRead32 (BaseAddr + DW_MMC_CLKDIV));

      Status = DwMmcExecuteSingleTransaction (BaseAddr, Packet, &RintSts);
      if (!EFI_ERROR (Status)) {
        DwMmcSetSavedClkSel (BaseAddr, TestClkSel);
        Print (L"[DWMMC RX-TUNED] CMD=%u phase=%u CLKSEL=0x%x\n",
               CmdIdx, Phase, TestClkSel);
        break;
      }
    }

    if (EFI_ERROR (Status)) {
      // Restore the last known Samsung SDR timing after an unsuccessful sweep.
      MmioWrite32 (BaseAddr + EXYNOS_DWMMC_CLKSEL, BaseClkSel);
      ArmDataSynchronizationBarrier ();
      ClockStatus = DwMmcUpdateClock (BaseAddr);
      if (EFI_ERROR (ClockStatus)) {
        Print (L"[DWMMC CLK-RESTORE-ERR] status=0x%lx\n",
               (UINT64)ClockStatus);
      }
    }
  }

  if (EFI_ERROR (Status) && BaseAddr == EXYNOS_DWMMC0_BASE && Packet->InDataBuffer != NULL) {
    UINT8 *B = (UINT8 *)Packet->InDataBuffer;
    UINT32 SecCnt = (UINT32)B[212] | ((UINT32)B[213] << 8) | ((UINT32)B[214] << 16) | ((UINT32)B[215] << 24);
    UINT32 FirstFf = 0xFFFF;
    UINT32 i;
    for (i = 0; i < Packet->InTransferLength; i++) {
      if (B[i] == 0xFF) {
        FirstFf = i;
        break;
      }
    }
    Print (L"[DW0 CMD%d err] St=0x%x RINT=0x%x CTYPE=0x%x UHS_EXT=0x%x DDR=0x%x Rev=%u SecCnt=0x%x FirstFF=%u\n",
           CmdIdx, (UINT32)Status, RintSts,
           MmioRead32 (BaseAddr + DW_MMC_CTYPE),
           MmioRead32 (BaseAddr + EXYNOS_DWMMC_UHS_REG_EXT),
           MmioRead32 (BaseAddr + EXYNOS_DWMMC_DDR_REG),
           B[192], SecCnt, FirstFf);
    if (Packet->InTransferLength >= 260) {
      Print (L"[DW0 CMD%d dump] B60..67=%02x%02x%02x%02x_%02x%02x%02x%02x B252..259=%02x%02x%02x%02x_%02x%02x%02x%02x\n",
             CmdIdx,
             B[60], B[61], B[62], B[63], B[64], B[65], B[66], B[67],
             B[252], B[253], B[254], B[255], B[256], B[257], B[258], B[259]);
    }
  }

  if (!EFI_ERROR (Status) && BaseAddr == EXYNOS_DWMMC0_BASE && CmdIdx == 8 && Packet->InTransferLength == 512) {
    UINT8 *B = (UINT8 *)Packet->InDataBuffer;
    UINT32 SecCnt = (UINT32)B[212] | ((UINT32)B[213] << 8) | ((UINT32)B[214] << 16) | ((UINT32)B[215] << 24);
    Print (L"[DW0 CMD8 OK] CLKSEL=0x%x CTYPE=0x%x Rev=%u SecCnt=0x%x\n",
           MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL),
           MmioRead32 (BaseAddr + DW_MMC_CTYPE),
           B[192], SecCnt);
  }

  Packet->TransactionStatus = Status;
  return Status;
}
