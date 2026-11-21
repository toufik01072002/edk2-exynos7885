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

#define DW_MMC_IDMAC_MAX_DESC        256
#define DW_MMC_IDMAC_MAX_CHUNK       4096
#define DW_MMC_DMA_BOUNCE_BYTES      (DW_MMC_IDMAC_MAX_DESC * DW_MMC_IDMAC_MAX_CHUNK)
#define DW_MMC_DMA_MAX_PHYS_ADDR     0x87FFFFFFULL

typedef struct {
  BOOLEAN                Initialized;
  DW_MMC_IDMAC_DESC64    *DescRing;
  EFI_PHYSICAL_ADDRESS   DescRingPhys;
  UINT8                  *BounceBuf;
  EFI_PHYSICAL_ADDRESS   BounceBufPhys;
} DW_MMC_DMA_CONTEXT;

STATIC DW_MMC_DMA_CONTEXT mDmaCtx[EXYNOS_DWMMC_MAX_SLOTS];
STATIC UINT32             mSavedClkSel0 = 0x03040000;
STATIC UINT32             mSavedClkSel2 = 0x03040000;
STATIC BOOLEAN            mClkSel0Captured = FALSE;
STATIC BOOLEAN            mClkSel2Captured = FALSE;
STATIC BOOLEAN            mRxTuned0 = FALSE;
STATIC BOOLEAN            mRxTuned2 = FALSE;
STATIC UINT32             mLastLoggedErr = 0;

STATIC
UINTN
DwMmcGetSlotIndex (
  IN UINTN BaseAddr
  )
{
  return (BaseAddr == EXYNOS_DWMMC0_BASE) ? 0 : 1;
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
EFI_STATUS
DwMmcEnsureDmaContext (
  IN UINTN BaseAddr
  )
{
  UINTN                Slot;
  DW_MMC_DMA_CONTEXT   *Ctx;
  EFI_STATUS           Status;
  EFI_PHYSICAL_ADDRESS Addr;
  UINTN                DescPages;
  UINTN                BouncePages;

  Slot = DwMmcGetSlotIndex (BaseAddr);
  Ctx  = &mDmaCtx[Slot];

  if (Ctx->Initialized) {
    return EFI_SUCCESS;
  }

  DescPages   = EFI_SIZE_TO_PAGES (sizeof (DW_MMC_IDMAC_DESC64) * DW_MMC_IDMAC_MAX_DESC);
  BouncePages = EFI_SIZE_TO_PAGES (DW_MMC_DMA_BOUNCE_BYTES);

  Addr   = DW_MMC_DMA_MAX_PHYS_ADDR;
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiBootServicesData,
                  DescPages,
                  &Addr
                  );
  if (EFI_ERROR (Status)) {
    Print (L"[DWMMC 0x%x] IDMAC desc alloc failed: %r\n", (UINT32)BaseAddr, Status);
    return Status;
  }
  Ctx->DescRingPhys = Addr;
  Ctx->DescRing     = (DW_MMC_IDMAC_DESC64 *)(UINTN)Addr;
  ZeroMem (Ctx->DescRing, EFI_PAGES_TO_SIZE (DescPages));
  WriteBackInvalidateDataCacheRange (Ctx->DescRing, EFI_PAGES_TO_SIZE (DescPages));

  Addr   = DW_MMC_DMA_MAX_PHYS_ADDR;
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiBootServicesData,
                  BouncePages,
                  &Addr
                  );
  if (EFI_ERROR (Status)) {
    Print (L"[DWMMC 0x%x] IDMAC bounce alloc failed: %r\n", (UINT32)BaseAddr, Status);
    return Status;
  }
  Ctx->BounceBufPhys = Addr;
  Ctx->BounceBuf     = (UINT8 *)(UINTN)Addr;
  ZeroMem (Ctx->BounceBuf, EFI_PAGES_TO_SIZE (BouncePages));
  WriteBackInvalidateDataCacheRange (Ctx->BounceBuf, EFI_PAGES_TO_SIZE (BouncePages));

  Ctx->Initialized = TRUE;
  return EFI_SUCCESS;
}

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
VOID
DwMmcEnableIdmac (
  IN UINTN BaseAddr
  )
{
  UINTN                Slot;
  DW_MMC_DMA_CONTEXT   *Ctx;
  UINT32               Ctrl;

  Slot = DwMmcGetSlotIndex (BaseAddr);
  Ctx  = &mDmaCtx[Slot];

  MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_SWR);
  DwMmcWaitForClear (BaseAddr, DW_MMC_BMOD, DW_MMC_BMOD_SWR, 10000);

  MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
  MmioWrite32 (
    BaseAddr + DW_MMC_IDINTEN,
    DW_MMC_IDMAC_INT_TI | DW_MMC_IDMAC_INT_RI | DW_MMC_IDMAC_INT_FBE |
    DW_MMC_IDMAC_INT_DU | DW_MMC_IDMAC_INT_CES | DW_MMC_IDMAC_INT_NI |
    DW_MMC_IDMAC_INT_AI
    );

  if (Ctx->Initialized) {
    MmioWrite32 (BaseAddr + DW_MMC_DBADDRL, (UINT32)(Ctx->DescRingPhys & 0xFFFFFFFFULL));
    MmioWrite32 (BaseAddr + DW_MMC_DBADDRU, (UINT32)(Ctx->DescRingPhys >> 32));
  }

  MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_DE | DW_MMC_BMOD_FB);

  Ctrl = MmioRead32 (BaseAddr + DW_MMC_CTRL);
  Ctrl &= ~DW_MMC_CTRL_INT_ENABLE;
  Ctrl |= DW_MMC_CTRL_USE_IDMAC | DW_MMC_CTRL_DMA_ENABLE;
  MmioWrite32 (BaseAddr + DW_MMC_CTRL, Ctrl);
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

  DwMmcEnableIdmac (BaseAddr);

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
  UINT32     ClkSelBoot;
  UINT32     Ctrl;
  EFI_STATUS Status;

  Status = DwMmcEnsureDmaContext (BaseAddr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ClkSelBoot = MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL);
  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    if (!mClkSel0Captured) {
      mSavedClkSel0    = (ClkSelBoot != 0) ? ClkSelBoot : 0x03040000;
      mClkSel0Captured = TRUE;
    }
  } else {
    if (!mClkSel2Captured) {
      mSavedClkSel2    = (ClkSelBoot != 0) ? ClkSelBoot : 0x03040000;
      mClkSel2Captured = TRUE;
    }
  }

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
    Print (L"[DWMMC 0x%x] controller reset timeout\n", (UINT32)BaseAddr);
    return Status;
  }

  MmioWrite32 (BaseAddr + EXYNOS_DWMMC_CLKSEL, DwMmcGetSavedClkSel (BaseAddr));
  MmioWrite32 (BaseAddr + DW_MMC_PWREN, 1);
  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_INTMASK, 0);
  MmioWrite32 (BaseAddr + DW_MMC_TMOUT, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_CTYPE, DW_MMC_CTYPE_1BIT);
  MmioWrite32 (BaseAddr + DW_MMC_FIFOTH, (0x2 << 28) | (15 << 16) | 16);

  DwMmcEnableIdmac (BaseAddr);

  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    Print (L"[DW0] VERID=0x%x HCON=0x%x CLKSEL=0x%x\n",
           MmioRead32 (BaseAddr + DW_MMC_VERID),
           MmioRead32 (BaseAddr + DW_MMC_HCON),
           MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL));
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
  } else if (ClockFreqKhz <= 25000) {
    Div = 16;
  } else {
    Div = 8;
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

  if (BaseAddr != EXYNOS_DWMMC0_BASE) {
    if (BusWidth == 4) {
      Ctype = DW_MMC_CTYPE_4BIT;
    } else if (BusWidth == 8) {
      Ctype = DW_MMC_CTYPE_8BIT;
    }
  }

  MmioWrite32 (BaseAddr + DW_MMC_CTYPE, Ctype);
  return DwMmcUpdateClock (BaseAddr);
}

STATIC
EFI_STATUS
DwMmcPrepareIdmacDescriptors (
  IN UINTN   BaseAddr,
  IN UINT32  TotalBytes,
  IN BOOLEAN IsRead,
  IN VOID    *UserBuf
  )
{
  UINTN                Slot;
  DW_MMC_DMA_CONTEXT   *Ctx;
  UINT32               NumDescs;
  UINT32               Remaining;
  UINT32               Offset;
  UINT32               i;

  if (TotalBytes > DW_MMC_DMA_BOUNCE_BYTES) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Slot = DwMmcGetSlotIndex (BaseAddr);
  Ctx  = &mDmaCtx[Slot];

  if (!Ctx->Initialized) {
    return EFI_NOT_READY;
  }

  if (!IsRead && UserBuf != NULL) {
    CopyMem (Ctx->BounceBuf, UserBuf, TotalBytes);
  }
  WriteBackInvalidateDataCacheRange (Ctx->BounceBuf, ALIGN_VALUE (TotalBytes, 64));

  NumDescs  = (TotalBytes + DW_MMC_IDMAC_MAX_CHUNK - 1) / DW_MMC_IDMAC_MAX_CHUNK;
  Remaining = TotalBytes;
  Offset    = 0;

  for (i = 0; i < NumDescs; i++) {
    UINT32               Chunk   = (Remaining > DW_MMC_IDMAC_MAX_CHUNK) ? DW_MMC_IDMAC_MAX_CHUNK : Remaining;
    EFI_PHYSICAL_ADDRESS BufAddr = Ctx->BounceBufPhys + Offset;
    UINT32               Des0    = IDMAC_DES0_OWN | IDMAC_DES0_DIC;

    if (i == 0) {
      Des0 |= IDMAC_DES0_FS;
    }
    if (i == (NumDescs - 1)) {
      Des0 |= IDMAC_DES0_LD | IDMAC_DES0_ER;
      Des0 &= ~IDMAC_DES0_DIC;
    }

    Ctx->DescRing[i].Des0          = Des0;
    Ctx->DescRing[i].Des1          = 0;
    Ctx->DescRing[i].Des2          = IDMAC_SET_BUFFER1_SIZE (Chunk);
    Ctx->DescRing[i].Des3          = 0;
    Ctx->DescRing[i].Buffer1AddrLo = (UINT32)(BufAddr & 0xFFFFFFFFULL);
    Ctx->DescRing[i].Buffer1AddrHi = (UINT32)(BufAddr >> 32);
    Ctx->DescRing[i].NextDescAddrLo = 0;
    Ctx->DescRing[i].NextDescAddrHi = 0;

    Remaining -= Chunk;
    Offset    += Chunk;
  }

  WriteBackInvalidateDataCacheRange (
    Ctx->DescRing,
    ALIGN_VALUE (NumDescs * sizeof (DW_MMC_IDMAC_DESC64), 64)
    );
  ArmDataSynchronizationBarrier ();

  MmioWrite32 (BaseAddr + DW_MMC_DBADDRL, (UINT32)(Ctx->DescRingPhys & 0xFFFFFFFFULL));
  MmioWrite32 (BaseAddr + DW_MMC_DBADDRU, (UINT32)(Ctx->DescRingPhys >> 32));
  MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_DE | DW_MMC_BMOD_FB);

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
DwMmcExecuteSingleTransaction (
  IN     UINTN                               BaseAddr,
  IN OUT EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *Packet,
  IN     BOOLEAN                             LogErrors
  )
{
  UINT32             CmdReg = DW_MMC_CMD_START | BIT29;
  UINT32             RintSts;
  UINT32             IdSts;
  UINT32             Timeout;
  EFI_STATUS         Status;
  BOOLEAN            IsRead = FALSE;
  UINT32             TotalBytes = 0;
  VOID               *UserBuf = NULL;
  UINT16             CmdIdx;
  UINT32             CmdArg;
  BOOLEAN            IsDeselect;
  UINTN              Slot;
  DW_MMC_DMA_CONTEXT *Ctx;

  CmdIdx     = Packet->SdMmcCmdBlk->CommandIndex;
  CmdArg     = Packet->SdMmcCmdBlk->CommandArgument;
  IsDeselect = (CmdIdx == 7 && CmdArg == 0);
  Slot       = DwMmcGetSlotIndex (BaseAddr);
  Ctx        = &mDmaCtx[Slot];

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
      DwMmcRecoverController (BaseAddr, CmdIdx);
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

  if (TotalBytes > 0) {
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

    MmioOr32 (BaseAddr + DW_MMC_CTRL, DW_MMC_CTRL_USE_IDMAC | DW_MMC_CTRL_DMA_ENABLE);

    Status = DwMmcPrepareIdmacDescriptors (BaseAddr, TotalBytes, IsRead, UserBuf);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    MmioWrite32 (BaseAddr + DW_MMC_BLKSIZ, (TotalBytes < 512) ? TotalBytes : 512);
    MmioWrite32 (BaseAddr + DW_MMC_BYTCNT, TotalBytes);

    if (CmdIdx == 18 || CmdIdx == 25) {
      CmdReg |= DW_MMC_CMD_SEND_STOP;
    }
  }

  MmioWrite32 (BaseAddr + DW_MMC_CMDARG, CmdArg);
  ArmDataSynchronizationBarrier ();
  MmioWrite32 (BaseAddr + DW_MMC_CMD, CmdReg);

  if (TotalBytes > 0) {
    MmioWrite32 (BaseAddr + DW_MMC_PLDMND, 1);
  }

  Timeout = 200000;
  while (Timeout > 0) {
    RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
    if (RintSts & DW_MMC_INT_ERROR) {
      if (LogErrors && mLastLoggedErr < 8) {
        Print (L"[DWMMC 0x%x] CMD%d(0x%x) err RINTSTS=0x%x\n",
               (UINT32)BaseAddr, CmdIdx, CmdArg, RintSts);
        mLastLoggedErr++;
      }
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
      return (RintSts & DW_MMC_INT_DCRC) ? EFI_CRC_ERROR : EFI_DEVICE_ERROR;
    }
    if (RintSts & DW_MMC_INT_CMD_DONE) {
      break;
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

  if (TotalBytes > 0) {
    Timeout = 500000;
    while (Timeout > 0) {
      RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
      IdSts   = MmioRead32 (BaseAddr + DW_MMC_IDSTS);

      if ((IdSts & (DW_MMC_IDMAC_INT_FBE | DW_MMC_IDMAC_INT_DU)) ||
          (RintSts & DW_MMC_INT_ERROR)) {
        if (IsRead && UserBuf != NULL) {
          InvalidateDataCacheRange (Ctx->BounceBuf, ALIGN_VALUE (TotalBytes, 64));
          CopyMem (UserBuf, Ctx->BounceBuf, TotalBytes);
        }
        if (LogErrors) {
          UINT8 *B = Ctx->BounceBuf;
          Print (L"[DWMMC 0x%x] CMD%d data err IDSTS=0x%x RINT=0x%x B0..3=%02x%02x%02x%02x\n",
                 (UINT32)BaseAddr, CmdIdx, IdSts, RintSts, B[0], B[1], B[2], B[3]);
        }
        MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
        MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
        return (RintSts & DW_MMC_INT_DCRC) ? EFI_CRC_ERROR : EFI_DEVICE_ERROR;
      }

      if ((RintSts & DW_MMC_INT_DTO) &&
          (IdSts & (DW_MMC_IDMAC_INT_RI | DW_MMC_IDMAC_INT_TI))) {
        break;
      }

      MicroSecondDelay (1);
      Timeout--;
    }

    if (Timeout == 0) {
      if (LogErrors) {
        Print (L"[DWMMC 0x%x] CMD%d IDMAC TIMEOUT IDSTS=0x%x RINT=0x%x\n",
               (UINT32)BaseAddr,
               CmdIdx,
               MmioRead32 (BaseAddr + DW_MMC_IDSTS),
               MmioRead32 (BaseAddr + DW_MMC_RINTSTS));
      }
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
      MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
      return EFI_TIMEOUT;
    }

    if (IsRead && UserBuf != NULL) {
      InvalidateDataCacheRange (Ctx->BounceBuf, ALIGN_VALUE (TotalBytes, 64));
      CopyMem (UserBuf, Ctx->BounceBuf, TotalBytes);

      if (BaseAddr == EXYNOS_DWMMC0_BASE && CmdIdx == 8 && TotalBytes == 512) {
        UINT8  *B      = (UINT8 *)UserBuf;
        UINT32 SecCnt  = (UINT32)B[212] |
                         ((UINT32)B[213] << 8) |
                         ((UINT32)B[214] << 16) |
                         ((UINT32)B[215] << 24);
        Print (L"[DW0 CMD8 OK] CLKSEL=0x%x Rev=%u SecCnt=0x%x B0..3=%02x%02x%02x%02x\n",
               MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL),
               B[192],
               SecCnt,
               B[0], B[1], B[2], B[3]);
      }
    }
  }

  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
  return EFI_SUCCESS;
}

EFI_STATUS
DwMmcExecCommand (
  IN     UINTN                               BaseAddr,
  IN OUT EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *Packet
  )
{
  EFI_STATUS Status;
  UINT16     CmdIdx;
  UINT32     CmdArg;
  BOOLEAN    IsRead;
  BOOLEAN    AlreadyTuned;

  if (Packet == NULL || Packet->SdMmcCmdBlk == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  CmdIdx = Packet->SdMmcCmdBlk->CommandIndex;
  CmdArg = Packet->SdMmcCmdBlk->CommandArgument;

  // Filtrer CMD6 BUS_WIDTH (183) et HS_TIMING (185) sur DWMMC0 pour rester en 1-bit SDR stable
  if (CmdIdx == 6 && BaseAddr == EXYNOS_DWMMC0_BASE) {
    UINT8 Index = (CmdArg >> 16) & 0xFF;
    UINT8 Value = (CmdArg >> 8) & 0xFF;
    if ((Index == 183 || Index == 185) && Value != 0) {
      if (Packet->SdMmcStatusBlk != NULL) {
        Packet->SdMmcStatusBlk->Resp0 = 0x00000900;
      }
      Packet->TransactionStatus = EFI_SUCCESS;
      return EFI_SUCCESS;
    }
  }

  IsRead       = (Packet->InDataBuffer != NULL && Packet->InTransferLength > 0);
  AlreadyTuned = (BaseAddr == EXYNOS_DWMMC0_BASE) ? mRxTuned0 : mRxTuned2;

  Status = DwMmcExecuteSingleTransaction (BaseAddr, Packet, TRUE);

  if (Status == EFI_CRC_ERROR && IsRead && !AlreadyTuned) {
    UINT32 BaseClkSel = DwMmcGetSavedClkSel (BaseAddr) & ~0x7U;
    UINT32 Phase;

    if (BaseAddr == EXYNOS_DWMMC0_BASE) {
      mRxTuned0 = TRUE;
    } else {
      mRxTuned2 = TRUE;
    }

    Print (L"[DWMMC 0x%x] CMD%d DCRC -> tuning RX phase (0..7)...\n",
           (UINT32)BaseAddr, CmdIdx);

    for (Phase = 0; Phase < 8; Phase++) {
      UINT32 Candidate = BaseClkSel | Phase;
      MmioWrite32 (BaseAddr + EXYNOS_DWMMC_CLKSEL, Candidate);
      Status = DwMmcExecuteSingleTransaction (BaseAddr, Packet, FALSE);
      Print (L"[DWMMC 0x%x] RX phase %u (0x%x) -> %r\n",
             (UINT32)BaseAddr, Phase, Candidate, Status);
      if (!EFI_ERROR (Status)) {
        DwMmcSetSavedClkSel (BaseAddr, Candidate);
        break;
      }
    }
  }

  Packet->TransactionStatus = Status;
  return Status;
}
