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

#define DW_MMC_IDMAC_DESC_COUNT   256
#define DW_MMC_IDMAC_MAX_CHUNK    4096
#define DW_MMC_DMA_BOUNCE_SIZE    (1024 * 1024)
#define DW_MMC_DMA_MAX_PHYS_ADDR  0x87FFFFFFULL

STATIC DW_MMC_IDMAC_DESC64 *mIdmacRing0 = NULL;
STATIC DW_MMC_IDMAC_DESC64 *mIdmacRing2 = NULL;
STATIC UINT8               *mDmaBounce0 = NULL;
STATIC UINT8               *mDmaBounce2 = NULL;

// Valeur CLKSEL native Exynos 7885 (DRV=4, DIV_RATIO=3, SAMPLE=0..7)
STATIC UINT32  mSavedClkSel0 = 0x03040000;
STATIC UINT32  mSavedClkSel2 = 0x03040000;
STATIC BOOLEAN mRxTuned0     = FALSE;
STATIC BOOLEAN mRxTuned2     = FALSE;
STATIC UINT32  mLastLoggedErr = 0;
STATIC UINT32  mCmdSeq        = 0;

STATIC
EFI_STATUS
DwMmcAllocateLowRam (
  IN  UINTN  Bytes,
  OUT VOID   **Buffer
  )
{
  EFI_STATUS           Status;
  EFI_PHYSICAL_ADDRESS PhysAddr;
  UINTN                Pages;

  if (Buffer == NULL || Bytes == 0) {
    return EFI_INVALID_PARAMETER;
  }

  Pages    = EFI_SIZE_TO_PAGES (Bytes);
  PhysAddr = DW_MMC_DMA_MAX_PHYS_ADDR;

  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiBootServicesData,
                  Pages,
                  &PhysAddr
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Buffer = (VOID *)(UINTN)PhysAddr;
  ZeroMem (*Buffer, EFI_PAGES_TO_SIZE (Pages));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
DwMmcEnsureIdmacResources (
  IN  UINTN               BaseAddr,
  OUT DW_MMC_IDMAC_DESC64 **RingOut,
  OUT UINT8               **BounceOut
  )
{
  EFI_STATUS          Status;
  DW_MMC_IDMAC_DESC64 **TargetRing;
  UINT8               **TargetBounce;
  UINTN               RingBytes;

  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    TargetRing   = &mIdmacRing0;
    TargetBounce = &mDmaBounce0;
  } else {
    TargetRing   = &mIdmacRing2;
    TargetBounce = &mDmaBounce2;
  }

  if (*TargetRing == NULL) {
    RingBytes = sizeof (DW_MMC_IDMAC_DESC64) * DW_MMC_IDMAC_DESC_COUNT;
    Status = DwMmcAllocateLowRam (RingBytes, (VOID **)TargetRing);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  if (*TargetBounce == NULL) {
    Status = DwMmcAllocateLowRam (DW_MMC_DMA_BOUNCE_SIZE, (VOID **)TargetBounce);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  *RingOut   = *TargetRing;
  *BounceOut = *TargetBounce;
  return EFI_SUCCESS;
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
  IN UINT32 Value
  )
{
  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    mSavedClkSel0 = Value;
  } else {
    mSavedClkSel2 = Value;
  }
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
DwMmcEnableIdmacHardware (
  IN UINTN BaseAddr
  )
{
  UINT32 Ctrl;

  MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
  MmioWrite32 (
    BaseAddr + DW_MMC_IDINTEN,
    DW_MMC_IDMAC_INT_TI | DW_MMC_IDMAC_INT_RI | DW_MMC_IDMAC_INT_FBE |
    DW_MMC_IDMAC_INT_DU | DW_MMC_IDMAC_INT_CES | DW_MMC_IDMAC_INT_NIS |
    DW_MMC_IDMAC_INT_AIS
    );
  MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_FB | DW_MMC_BMOD_DE);

  Ctrl = MmioRead32 (BaseAddr + DW_MMC_CTRL);
  Ctrl &= ~DW_MMC_CTRL_INT_ENABLE;
  Ctrl |= (DW_MMC_CTRL_USE_IDMAC | DW_MMC_CTRL_DMA_ENABLE);
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
  Ctrl |= (DW_MMC_CTRL_RESET | DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET);
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

  MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_SWR);
  DwMmcWaitForClear (BaseAddr, DW_MMC_BMOD, DW_MMC_BMOD_SWR, 10000);
  DwMmcEnableIdmacHardware (BaseAddr);

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
  UINT32              ClkSelBoot;
  UINT32              Ctrl;
  EFI_STATUS          Status;
  DW_MMC_IDMAC_DESC64 *Ring;
  UINT8               *Bounce;

  ClkSelBoot = MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL);
  if (ClkSelBoot != 0) {
    DwMmcSetSavedClkSel (BaseAddr, ClkSelBoot);
  } else {
    DwMmcSetSavedClkSel (BaseAddr, 0x03040000);
  }

  Status = DwMmcEnsureIdmacResources (BaseAddr, &Ring, &Bounce);
  if (EFI_ERROR (Status)) {
    return Status;
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
    return Status;
  }

  MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_SWR);
  DwMmcWaitForClear (BaseAddr, DW_MMC_BMOD, DW_MMC_BMOD_SWR, 10000);

  MmioWrite32 (BaseAddr + EXYNOS_DWMMC_CLKSEL, DwMmcGetSavedClkSel (BaseAddr));
  MmioWrite32 (BaseAddr + DW_MMC_PWREN, 1);
  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_INTMASK, 0);
  MmioWrite32 (BaseAddr + DW_MMC_TMOUT, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_CTYPE, DW_MMC_CTYPE_1BIT);
  MmioWrite32 (BaseAddr + DW_MMC_FIFOTH,
               (0x2 << 28) | (15 << 16) | 16);

  DwMmcEnableIdmacHardware (BaseAddr);

  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    Print (L"[DW0] VERID=0x%x HCON=0x%x CLKSEL=0x%x
",
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

  MmioWrite32 (BaseAddr + DW_MMC_CLKENA,
               DW_MMC_CLKENA_CCLK_ENABLE);

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
DwMmcExecuteSingleTransaction (
  IN     UINTN                               BaseAddr,
  IN OUT EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *Packet,
  OUT    UINT32                              *OutRintSts,
  OUT    UINT32                              *OutIdSts
  )
{
  UINT32              CmdReg = DW_MMC_CMD_START | BIT29;
  UINT32              RintSts;
  UINT32              IdSts;
  UINT32              Timeout;
  EFI_STATUS          Status;
  BOOLEAN             IsRead = FALSE;
  UINT32              TotalBytes = 0;
  UINT8               *UserBuf = NULL;
  UINT16              CmdIdx;
  UINT32              CmdArg;
  BOOLEAN             IsDeselect;
  DW_MMC_IDMAC_DESC64 *Ring = NULL;
  UINT8               *Bounce = NULL;
  UINTN               RingBytes = 0;
  UINTN               AlignedBytes = 0;

  if (OutRintSts != NULL) {
    *OutRintSts = 0;
  }
  if (OutIdSts != NULL) {
    *OutIdSts = 0;
  }

  CmdIdx     = Packet->SdMmcCmdBlk->CommandIndex;
  CmdArg     = Packet->SdMmcCmdBlk->CommandArgument;
  IsDeselect = (CmdIdx == 7 && CmdArg == 0);

  if (CmdIdx != 13 && CmdIdx != 12 && !IsDeselect) {
    CmdReg |= DW_MMC_CMD_WAIT_PRVDATA;
  }

  Status = DwMmcWaitForClear (
             BaseAddr, DW_MMC_CMD, DW_MMC_CMD_START, 200000
             );
  if (EFI_ERROR (Status)) {
    DwMmcRecoverController (BaseAddr, CmdIdx);
    Packet->TransactionStatus = EFI_TIMEOUT;
    return EFI_TIMEOUT;
  }

  if (CmdIdx != 13 && CmdIdx != 12 && !IsDeselect) {
    Status = DwMmcWaitForClear (
               BaseAddr, DW_MMC_STATUS,
               DW_MMC_STATUS_DATA_BUSY, 200000
               );
    if (EFI_ERROR (Status)) {
      DwMmcRecoverController (BaseAddr, CmdIdx);
      Packet->TransactionStatus = EFI_TIMEOUT;
      return EFI_TIMEOUT;
    }
  }

  if (CmdIdx == 7 || CmdIdx == 8 || CmdIdx == 17 || CmdIdx == 18) {
    if (MmioRead32 (BaseAddr + DW_MMC_CLKDIV) == 64) {
      Status = DwMmcSetClock (BaseAddr, 25000);
      if (EFI_ERROR (Status)) {
        Packet->TransactionStatus = Status;
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
    UserBuf    = (UINT8 *)Packet->InDataBuffer;
    CmdReg    |= DW_MMC_CMD_DATA_EXP;
  } else if (Packet->OutDataBuffer != NULL && Packet->OutTransferLength > 0) {
    IsRead     = FALSE;
    TotalBytes = Packet->OutTransferLength;
    UserBuf    = (UINT8 *)Packet->OutDataBuffer;
    CmdReg    |= (DW_MMC_CMD_DATA_EXP | DW_MMC_CMD_RW);
  }

  if (TotalBytes > 0) {
    UINT32 NumDesc;
    UINT32 i;
    UINT32 Remaining;
    UINTN  BufAddr;
    UINTN  RingAddr;

    if (TotalBytes > DW_MMC_DMA_BOUNCE_SIZE) {
      Packet->TransactionStatus = EFI_BAD_BUFFER_SIZE;
      return EFI_BAD_BUFFER_SIZE;
    }

    Status = DwMmcEnsureIdmacResources (BaseAddr, &Ring, &Bounce);
    if (EFI_ERROR (Status)) {
      Packet->TransactionStatus = Status;
      return Status;
    }

    MmioOr32 (BaseAddr + DW_MMC_CTRL,
              DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET);
    Status = DwMmcWaitForClear (
               BaseAddr, DW_MMC_CTRL,
               DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET, 100000
               );
    if (EFI_ERROR (Status)) {
      Packet->TransactionStatus = Status;
      return Status;
    }

    MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_SWR);
    DwMmcWaitForClear (BaseAddr, DW_MMC_BMOD, DW_MMC_BMOD_SWR, 10000);
    DwMmcEnableIdmacHardware (BaseAddr);

    AlignedBytes = (TotalBytes + 63) & ~63U;
    if (IsRead) {
      ZeroMem (Bounce, AlignedBytes);
    } else {
      CopyMem (Bounce, UserBuf, TotalBytes);
    }

    NumDesc = (TotalBytes + DW_MMC_IDMAC_MAX_CHUNK - 1) / DW_MMC_IDMAC_MAX_CHUNK;
    if (NumDesc > DW_MMC_IDMAC_DESC_COUNT) {
      Packet->TransactionStatus = EFI_BAD_BUFFER_SIZE;
      return EFI_BAD_BUFFER_SIZE;
    }

    RingBytes = sizeof (DW_MMC_IDMAC_DESC64) * NumDesc;
    ZeroMem (Ring, RingBytes);

    Remaining = TotalBytes;
    BufAddr   = (UINTN)Bounce;
    RingAddr  = (UINTN)Ring;

    for (i = 0; i < NumDesc; i++) {
      UINT32 Chunk = (Remaining > DW_MMC_IDMAC_MAX_CHUNK) ? DW_MMC_IDMAC_MAX_CHUNK : Remaining;
      UINT32 Des0  = IDMAC_DES0_OWN | IDMAC_DES0_DIC;

      if (i == 0) {
        Des0 |= IDMAC_DES0_FS;
      }
      if (i == NumDesc - 1) {
        Des0 &= ~IDMAC_DES0_DIC;
        Des0 |= (IDMAC_DES0_LD | IDMAC_DES0_ER);
        Ring[i].NextDescAddrLo = 0;
        Ring[i].NextDescAddrHi = 0;
      } else {
        UINTN NextAddr = RingAddr + ((i + 1) * sizeof (DW_MMC_IDMAC_DESC64));
        Des0 |= IDMAC_DES0_CH;
        Ring[i].NextDescAddrLo = (UINT32)(NextAddr & 0xFFFFFFFFULL);
        Ring[i].NextDescAddrHi = (UINT32)(NextAddr >> 32);
      }

      Ring[i].Des0          = Des0;
      Ring[i].Des1          = 0;
      Ring[i].Des2          = IDMAC_SET_BUFFER1_SIZE (Chunk);
      Ring[i].Des3          = 0;
      Ring[i].Buffer1AddrLo = (UINT32)(BufAddr & 0xFFFFFFFFULL);
      Ring[i].Buffer1AddrHi = (UINT32)(BufAddr >> 32);

      BufAddr   += Chunk;
      Remaining -= Chunk;
    }

    WriteBackInvalidateDataCacheRange (Bounce, AlignedBytes);
    WriteBackInvalidateDataCacheRange (Ring, RingBytes);
    ArmDataSynchronizationBarrier ();

    MmioWrite32 (BaseAddr + DW_MMC_DBADDRL, (UINT32)(RingAddr & 0xFFFFFFFFULL));
    MmioWrite32 (BaseAddr + DW_MMC_DBADDRU, (UINT32)(RingAddr >> 32));
    MmioWrite32 (BaseAddr + DW_MMC_BLKSIZ, (TotalBytes < 512) ? TotalBytes : 512);
    MmioWrite32 (BaseAddr + DW_MMC_BYTCNT, TotalBytes);

    if (CmdIdx == 18 || CmdIdx == 25) {
      CmdReg |= DW_MMC_CMD_SEND_STOP;
    }
  }

  MmioWrite32 (BaseAddr + DW_MMC_CMDARG, CmdArg);
  MmioWrite32 (BaseAddr + DW_MMC_CMD, CmdReg);

  if (TotalBytes > 0) {
    MmioWrite32 (BaseAddr + DW_MMC_PLDMND, 1);
  }

  Timeout = 300000;
  while (Timeout > 0) {
    RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
    if (RintSts & DW_MMC_INT_ERROR) {
      if (OutRintSts != NULL) {
        *OutRintSts = RintSts;
      }
      if (OutIdSts != NULL) {
        *OutIdSts = MmioRead32 (BaseAddr + DW_MMC_IDSTS);
      }
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
      MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
      Packet->TransactionStatus = EFI_DEVICE_ERROR;
      return EFI_DEVICE_ERROR;
    }
    if (RintSts & DW_MMC_INT_CMD_DONE) {
      break;
    }
    MicroSecondDelay (1);
    Timeout--;
  }

  if ((MmioRead32 (BaseAddr + DW_MMC_RINTSTS) & DW_MMC_INT_CMD_DONE) == 0) {
    if (OutRintSts != NULL) {
      *OutRintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
    }
    DwMmcRecoverController (BaseAddr, CmdIdx);
    Packet->TransactionStatus = EFI_TIMEOUT;
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

  if (TotalBytes > 0 && Bounce != NULL && UserBuf != NULL) {
    Timeout = 500000;
    while (Timeout > 0) {
      RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
      IdSts   = MmioRead32 (BaseAddr + DW_MMC_IDSTS);

      if (OutRintSts != NULL) {
        *OutRintSts = RintSts;
      }
      if (OutIdSts != NULL) {
        *OutIdSts = IdSts;
      }

      if ((IdSts & (DW_MMC_IDMAC_INT_FBE | DW_MMC_IDMAC_INT_DU)) ||
          (RintSts & DW_MMC_INT_ERROR)) {
        if (IsRead) {
          InvalidateDataCacheRange (Bounce, AlignedBytes);
          CopyMem (UserBuf, Bounce, TotalBytes);
        }
        MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
        MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
        Packet->TransactionStatus = EFI_DEVICE_ERROR;
        return EFI_DEVICE_ERROR;
      }

      if ((RintSts & DW_MMC_INT_DTO) &&
          ((IdSts & (DW_MMC_IDMAC_INT_RI | DW_MMC_IDMAC_INT_TI)) != 0)) {
        break;
      }

      MicroSecondDelay (1);
      Timeout--;
    }

    if (Timeout == 0) {
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
      MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
      Packet->TransactionStatus = EFI_TIMEOUT;
      return EFI_TIMEOUT;
    }

    if (IsRead) {
      InvalidateDataCacheRange (Bounce, AlignedBytes);
      CopyMem (UserBuf, Bounce, TotalBytes);
    }
  }

  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
  Packet->TransactionStatus = EFI_SUCCESS;
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
  UINT32     RintSts = 0;
  UINT32     IdSts   = 0;

  if (Packet == NULL || Packet->SdMmcCmdBlk == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  CmdIdx = Packet->SdMmcCmdBlk->CommandIndex;
  CmdArg = Packet->SdMmcCmdBlk->CommandArgument;

  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    mCmdSeq++;
  }

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

  Status = DwMmcExecuteSingleTransaction (BaseAddr, Packet, &RintSts, &IdSts);

  // Auto-tuning unique de la phase RX (CLKSEL bits 2:0 = 0..7) sur DCRC en lecture
  if (EFI_ERROR (Status) &&
      Packet->InDataBuffer != NULL &&
      Packet->InTransferLength > 0 &&
      (RintSts & DW_MMC_INT_DCRC)) {
    BOOLEAN *TunedFlag = (BaseAddr == EXYNOS_DWMMC0_BASE) ? &mRxTuned0 : &mRxTuned2;
    UINT8   *U = (UINT8 *)Packet->InDataBuffer;

    if (BaseAddr == EXYNOS_DWMMC0_BASE && Packet->InTransferLength == 512) {
      UINT32 SecCnt = (UINT32)U[212] | ((UINT32)U[213] << 8) |
                      ((UINT32)U[214] << 16) | ((UINT32)U[215] << 24);
      Print (L"[DW0 CMD8 DCRC] CLKSEL=0x%x RINT=0x%x Rev=%u SecCnt=0x%x B0..3=%02x%02x%02x%02x\n",
             DwMmcGetSavedClkSel (BaseAddr), RintSts, U[192], SecCnt, U[0], U[1], U[2], U[3]);
    }

    if (!(*TunedFlag)) {
      UINT32 BaseClkSel = DwMmcGetSavedClkSel (BaseAddr) & ~0x7U;
      UINT32 Phase;

      *TunedFlag = TRUE;

      for (Phase = 0; Phase < 8; Phase++) {
        UINT32 Candidate = BaseClkSel | Phase;
        UINT32 TryRint   = 0;
        UINT32 TryIdSts  = 0;

        DwMmcSetSavedClkSel (BaseAddr, Candidate);
        DwMmcRecoverController (BaseAddr, CmdIdx);

        Status = DwMmcExecuteSingleTransaction (BaseAddr, Packet, &TryRint, &TryIdSts);
        Print (L"[DWMMC 0x%x] RX tune phase %u (0x%x) -> Status=0x%x RINT=0x%x\n",
               (UINT32)BaseAddr, Phase, Candidate, (UINT32)Status, TryRint);

        if (!EFI_ERROR (Status)) {
          RintSts = TryRint;
          IdSts   = TryIdSts;
          break;
        }
      }
    }
  }

  if (EFI_ERROR (Status)) {
    if (mLastLoggedErr < 12 &&
        (CmdIdx == 8 || CmdIdx == 17 || CmdIdx == 18 || CmdIdx == 13)) {
      Print (L"[DWMMC 0x%x] CMD%d err Status=0x%x IDSTS=0x%x RINT=0x%x\n",
             (UINT32)BaseAddr, CmdIdx, (UINT32)Status, IdSts, RintSts);
      mLastLoggedErr++;
    }
    return Status;
  }

  if (BaseAddr == EXYNOS_DWMMC0_BASE && CmdIdx == 8 && Packet->InTransferLength == 512) {
    UINT8  *U = (UINT8 *)Packet->InDataBuffer;
    UINT32 SecCnt = (UINT32)U[212] |
                    ((UINT32)U[213] << 8) |
                    ((UINT32)U[214] << 16) |
                    ((UINT32)U[215] << 24);
    Print (L"[DW0 CMD8 OK] CLKSEL=0x%x Rev=%u SecCnt=0x%x B0..7=%02x%02x%02x%02x %02x%02x%02x%02x\n",
           DwMmcGetSavedClkSel (BaseAddr),
           U[192],
           SecCnt,
           U[0], U[1], U[2], U[3],
           U[4], U[5], U[6], U[7]);
  }

  return EFI_SUCCESS;
}
