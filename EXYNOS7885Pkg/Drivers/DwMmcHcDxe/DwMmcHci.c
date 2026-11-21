#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include "DwMmcHci.h"

#define IDMAC_RING_SIZE       256
#define IDMAC_MAX_CHUNK_BYTES 4096
#define IDMAC_BOUNCE_BYTES    (1024 * 1024)

STATIC UINT32 mLastLoggedErr  = 0;
STATIC UINT32 mSavedClkSel0   = 0x03040000;
STATIC UINT32 mSavedClkSel2   = 0x03040000;
STATIC BOOLEAN mRxTuned0      = FALSE;
STATIC BOOLEAN mRxTuned2      = FALSE;

STATIC DW_MMC_IDMAC_DESC64 *mIdmacRing0   = NULL;
STATIC DW_MMC_IDMAC_DESC64 *mIdmacRing2   = NULL;
STATIC VOID                *mBounceBuf0   = NULL;
STATIC VOID                *mBounceBuf2   = NULL;

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
  if (Val == 0) {
    Val = 0x03040000;
  }
  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    mSavedClkSel0 = Val;
  } else {
    mSavedClkSel2 = Val;
  }
}

STATIC
EFI_STATUS
DwMmcEnsureIdmacBuffers (
  IN  UINTN                BaseAddr,
  OUT DW_MMC_IDMAC_DESC64  **OutRing,
  OUT VOID                 **OutBounce
  )
{
  EFI_STATUS           Status;
  EFI_PHYSICAL_ADDRESS PhysAddr;
  UINTN                RingPages;
  UINTN                BouncePages;

  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    if (mIdmacRing0 == NULL) {
      RingPages = EFI_SIZE_TO_PAGES (IDMAC_RING_SIZE * sizeof (DW_MMC_IDMAC_DESC64));
      PhysAddr  = 0x87FFFFFFULL;
      Status    = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, RingPages, &PhysAddr);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      mIdmacRing0 = (DW_MMC_IDMAC_DESC64 *)(UINTN)PhysAddr;
      ZeroMem (mIdmacRing0, EFI_PAGES_TO_SIZE (RingPages));
    }
    if (mBounceBuf0 == NULL) {
      BouncePages = EFI_SIZE_TO_PAGES (IDMAC_BOUNCE_BYTES);
      PhysAddr    = 0x87FFFFFFULL;
      Status      = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, BouncePages, &PhysAddr);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      mBounceBuf0 = (VOID *)(UINTN)PhysAddr;
      ZeroMem (mBounceBuf0, EFI_PAGES_TO_SIZE (BouncePages));
    }
    *OutRing   = mIdmacRing0;
    *OutBounce = mBounceBuf0;
    return EFI_SUCCESS;
  } else {
    if (mIdmacRing2 == NULL) {
      RingPages = EFI_SIZE_TO_PAGES (IDMAC_RING_SIZE * sizeof (DW_MMC_IDMAC_DESC64));
      PhysAddr  = 0x87FFFFFFULL;
      Status    = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, RingPages, &PhysAddr);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      mIdmacRing2 = (DW_MMC_IDMAC_DESC64 *)(UINTN)PhysAddr;
      ZeroMem (mIdmacRing2, EFI_PAGES_TO_SIZE (RingPages));
    }
    if (mBounceBuf2 == NULL) {
      BouncePages = EFI_SIZE_TO_PAGES (IDMAC_BOUNCE_BYTES);
      PhysAddr    = 0x87FFFFFFULL;
      Status      = gBS->AllocatePages (AllocateMaxAddress, EfiBootServicesData, BouncePages, &PhysAddr);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      mBounceBuf2 = (VOID *)(UINTN)PhysAddr;
      ZeroMem (mBounceBuf2, EFI_PAGES_TO_SIZE (BouncePages));
    }
    *OutRing   = mIdmacRing2;
    *OutBounce = mBounceBuf2;
    return EFI_SUCCESS;
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
DwMmcEnableIdmac (
  IN UINTN BaseAddr
  )
{
  UINT32 Ctrl;

  MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
  MmioWrite32 (
    BaseAddr + DW_MMC_IDINTEN,
    DW_MMC_IDMAC_INT_TI | DW_MMC_IDMAC_INT_RI | DW_MMC_IDMAC_INT_FBE |
    DW_MMC_IDMAC_INT_DU | DW_MMC_IDMAC_INT_CES | DW_MMC_IDMAC_INT_NI |
    BIT9
    );

  MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_DE | DW_MMC_BMOD_FB);

  Ctrl  = MmioRead32 (BaseAddr + DW_MMC_CTRL);
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

  Ctrl  = MmioRead32 (BaseAddr + DW_MMC_CTRL);
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
  DwMmcWaitForClear (BaseAddr, DW_MMC_BMOD, DW_MMC_BMOD_SWR, 50000);
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

  Print (L"[DWMMC 0x%x] CMD%u recovery timeout\n", (UINT32)BaseAddr, CmdIdx);
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
  UINT32     ClkSelInitial;
  UINT32     Ctrl;
  EFI_STATUS Status;

  ClkSelInitial = MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL);
  if (ClkSelInitial == 0) {
    ClkSelInitial = 0x03040000;
  }
  DwMmcSetSavedClkSel (BaseAddr, ClkSelInitial);

  MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_SWR);
  DwMmcWaitForClear (BaseAddr, DW_MMC_BMOD, DW_MMC_BMOD_SWR, 50000);
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
  OUT    UINT32                              *OutLastRintSts
  )
{
  UINT32               CmdReg = DW_MMC_CMD_START | BIT29;
  UINT32               RintSts = 0;
  UINT32               IdSts = 0;
  UINT32               Timeout;
  EFI_STATUS           Status;
  BOOLEAN              IsRead = FALSE;
  UINT32               TotalBytes = 0;
  VOID                 *UserBuf = NULL;
  UINT16               CmdIdx;
  UINT32               CmdArg;
  BOOLEAN              IsDeselect;
  DW_MMC_IDMAC_DESC64  *Ring = NULL;
  VOID                 *BounceBuf = NULL;
  UINTN                RingPhys = 0;
  UINTN                BouncePhys = 0;
  UINT32               NumDescs = 0;

  if (OutLastRintSts != NULL) {
    *OutLastRintSts = 0;
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
    Packet->TransactionStatus = EFI_TIMEOUT;
    return EFI_TIMEOUT;
  }

  if (CmdIdx != 13 && CmdIdx != 12 && !IsDeselect) {
    Status = DwMmcWaitForClear (BaseAddr, DW_MMC_STATUS, DW_MMC_STATUS_DATA_BUSY, 200000);
    if (EFI_ERROR (Status)) {
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

  MmioWrite32 (BaseAddr + EXYNOS_DWMMC_CLKSEL, DwMmcGetSavedClkSel (BaseAddr));
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
    if (TotalBytes > IDMAC_BOUNCE_BYTES) {
      Packet->TransactionStatus = EFI_BAD_BUFFER_SIZE;
      return EFI_BAD_BUFFER_SIZE;
    }

    Status = DwMmcEnsureIdmacBuffers (BaseAddr, &Ring, &BounceBuf);
    if (EFI_ERROR (Status)) {
      Packet->TransactionStatus = Status;
      return Status;
    }

    RingPhys   = (UINTN)Ring;
    BouncePhys = (UINTN)BounceBuf;

    MmioOr32 (BaseAddr + DW_MMC_CTRL, DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET);
    Status = DwMmcWaitForClear (
               BaseAddr, DW_MMC_CTRL,
               DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET, 100000
               );
    if (EFI_ERROR (Status)) {
      Packet->TransactionStatus = Status;
      return Status;
    }

    MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_SWR);
    DwMmcWaitForClear (BaseAddr, DW_MMC_BMOD, DW_MMC_BMOD_SWR, 50000);
    DwMmcEnableIdmac (BaseAddr);

    if (IsRead) {
      ZeroMem (BounceBuf, TotalBytes);
    } else {
      CopyMem (BounceBuf, UserBuf, TotalBytes);
    }

    NumDescs = (TotalBytes + IDMAC_MAX_CHUNK_BYTES - 1) / IDMAC_MAX_CHUNK_BYTES;
    if (NumDescs > IDMAC_RING_SIZE) {
      Packet->TransactionStatus = EFI_BAD_BUFFER_SIZE;
      return EFI_BAD_BUFFER_SIZE;
    }

    ZeroMem (Ring, NumDescs * sizeof (DW_MMC_IDMAC_DESC64));

    UINT32 Remaining = TotalBytes;
    UINTN  CurBuf    = BouncePhys;
    for (UINT32 i = 0; i < NumDescs; i++) {
      UINT32 Chunk = (Remaining > IDMAC_MAX_CHUNK_BYTES) ? IDMAC_MAX_CHUNK_BYTES : Remaining;
      UINT32 Flags = IDMAC_DES0_OWN | IDMAC_DES0_DIC;

      if (i == 0) {
        Flags |= IDMAC_DES0_FS;
      }
      if (i == (NumDescs - 1)) {
        Flags |= (IDMAC_DES0_LD | IDMAC_DES0_ER);
        Flags &= ~IDMAC_DES0_DIC;
      } else {
        Flags |= IDMAC_DES0_CH;
        UINTN NextDesc = RingPhys + ((i + 1) * sizeof (DW_MMC_IDMAC_DESC64));
        Ring[i].NextDescAddrLo = (UINT32)(NextDesc & 0xFFFFFFFFULL);
        Ring[i].NextDescAddrHi = (UINT32)(NextDesc >> 32);
      }

      Ring[i].Des0          = Flags;
      Ring[i].Des1          = 0;
      Ring[i].Des2          = IDMAC_SET_BUFFER1_SIZE (Chunk);
      Ring[i].Des3          = 0;
      Ring[i].Buffer1AddrLo = (UINT32)(CurBuf & 0xFFFFFFFFULL);
      Ring[i].Buffer1AddrHi = (UINT32)(CurBuf >> 32);

      CurBuf    += Chunk;
      Remaining -= Chunk;
    }

    WriteBackInvalidateDataCacheRange (Ring, NumDescs * sizeof (DW_MMC_IDMAC_DESC64));
    WriteBackInvalidateDataCacheRange (BounceBuf, TotalBytes);
    ArmDataSynchronizationBarrier ();

    MmioWrite32 (BaseAddr + DW_MMC_DBADDRL, (UINT32)(RingPhys & 0xFFFFFFFFULL));
    MmioWrite32 (BaseAddr + DW_MMC_DBADDRU, (UINT32)(RingPhys >> 32));
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

  Timeout = 200000;
  while (Timeout > 0) {
    RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
    if (RintSts & (DW_MMC_INT_RE | DW_MMC_INT_RCRC | DW_MMC_INT_RTO | DW_MMC_INT_HLE)) {
      if (OutLastRintSts != NULL) {
        *OutLastRintSts = RintSts;
      }
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
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

  if (TotalBytes > 0) {
    Timeout = 500000;
    while (Timeout > 0) {
      RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
      IdSts   = MmioRead32 (BaseAddr + DW_MMC_IDSTS);

      if ((IdSts & DW_MMC_IDMAC_INT_FBE) != 0) {
        if (OutLastRintSts != NULL) {
          *OutLastRintSts = RintSts;
        }
        MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
        MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
        Packet->TransactionStatus = EFI_DEVICE_ERROR;
        return EFI_DEVICE_ERROR;
      }

      if ((RintSts & DW_MMC_INT_DTO) != 0) {
        break;
      }

      if ((RintSts & (DW_MMC_INT_DRTO | DW_MMC_INT_HTO | DW_MMC_INT_SBE | DW_MMC_INT_EBE)) != 0) {
        if (OutLastRintSts != NULL) {
          *OutLastRintSts = RintSts;
        }
        MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
        MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
        Packet->TransactionStatus = EFI_DEVICE_ERROR;
        return EFI_DEVICE_ERROR;
      }

      MicroSecondDelay (1);
      Timeout--;
    }

    if (OutLastRintSts != NULL) {
      *OutLastRintSts = RintSts;
    }

    if (Timeout == 0) {
      Packet->TransactionStatus = EFI_TIMEOUT;
      return EFI_TIMEOUT;
    }

    if (IsRead) {
      InvalidateDataCacheRange (BounceBuf, TotalBytes);
      ArmDataSynchronizationBarrier ();
      CopyMem (UserBuf, BounceBuf, TotalBytes);
    }

    if ((RintSts & DW_MMC_INT_DCRC) != 0) {
      MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
      Packet->TransactionStatus = EFI_CRC_ERROR;
      return EFI_CRC_ERROR;
    }
  }

  MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
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
  UINT32     LastRintSts = 0;
  UINT16     CmdIdx;
  UINT32     CmdArg;
  BOOLEAN    *pTunedFlag;

  if (Packet == NULL || Packet->SdMmcCmdBlk == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  CmdIdx = Packet->SdMmcCmdBlk->CommandIndex;
  CmdArg = Packet->SdMmcCmdBlk->CommandArgument;

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

  Status = DwMmcExecuteSingleTransaction (BaseAddr, Packet, &LastRintSts);

  pTunedFlag = (BaseAddr == EXYNOS_DWMMC0_BASE) ? &mRxTuned0 : &mRxTuned2;

  if (Status == EFI_CRC_ERROR && Packet->InDataBuffer != NULL && !(*pTunedFlag)) {
    *pTunedFlag = TRUE;
    UINT32 BaseClkSel = DwMmcGetSavedClkSel (BaseAddr) & ~0x7U;

    if (BaseAddr == EXYNOS_DWMMC0_BASE && CmdIdx == 8 && Packet->InTransferLength == 512) {
      UINT8 *B = (UINT8 *)Packet->InDataBuffer;
      UINT32 SecCnt = B[212] | (B[213] << 8) | (B[214] << 16) | (B[215] << 24);
      Print (L"[DW0 CMD8 DCRC] Rev=%u SecCnt=0x%x B0..3=%02x%02x%02x%02x\n",
             B[192], SecCnt, B[0], B[1], B[2], B[3]);
    }

    for (UINT32 Phase = 0; Phase < 8; Phase++) {
      UINT32 Candidate = BaseClkSel | Phase;
      DwMmcSetSavedClkSel (BaseAddr, Candidate);
      Status = DwMmcExecuteSingleTransaction (BaseAddr, Packet, &LastRintSts);

      if (BaseAddr == EXYNOS_DWMMC0_BASE) {
        UINT8 *B = (UINT8 *)Packet->InDataBuffer;
        UINT32 SecCnt = (Packet->InTransferLength == 512)
                        ? (B[212] | (B[213] << 8) | (B[214] << 16) | (B[215] << 24))
                        : 0;
        Print (L"  [Tune P%u] St=0x%x RINT=0x%x Rev=%u Sec=0x%x\n",
               Phase, (UINT32)Status, LastRintSts,
               (Packet->InTransferLength == 512) ? B[192] : 0, SecCnt);
      }

      if (!EFI_ERROR (Status)) {
        break;
      }
    }
  }

  if (EFI_ERROR (Status) && mLastLoggedErr < 8) {
    Print (L"[DWMMC 0x%x] CMD%u fail St=0x%x RINT=0x%x CLKSEL=0x%x\n",
           (UINT32)BaseAddr, CmdIdx, (UINT32)Status, LastRintSts, DwMmcGetSavedClkSel (BaseAddr));
    mLastLoggedErr++;
  }

  if (!EFI_ERROR (Status) && BaseAddr == EXYNOS_DWMMC0_BASE && CmdIdx == 8 && Packet->InTransferLength == 512) {
    UINT8 *B = (UINT8 *)Packet->InDataBuffer;
    UINT32 SecCnt = B[212] | (B[213] << 8) | (B[214] << 16) | (B[215] << 24);
    Print (L"[DW0 CMD8 OK] CLKSEL=0x%x Rev=%u SecCnt=0x%x\n",
           DwMmcGetSavedClkSel (BaseAddr), B[192], SecCnt);
  }

  return Status;
}
