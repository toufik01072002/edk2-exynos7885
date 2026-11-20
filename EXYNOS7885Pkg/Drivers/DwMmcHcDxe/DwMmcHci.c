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

#define DW_MMC_IDMAC_RING_COUNT   256
#define DW_MMC_BOUNCE_BUFFER_SIZE (1024 * 1024)

STATIC UINT32               mLastLoggedErr = 0;
STATIC UINT32               mCmdSeq        = 0;
STATIC UINT32               mSavedClkSel0  = 0;
STATIC UINT32               mSavedClkSel2  = 0;
STATIC DW_MMC_IDMAC_DESC64 *mIdmacRing0    = NULL;
STATIC DW_MMC_IDMAC_DESC64 *mIdmacRing2    = NULL;
STATIC UINT8               *mBounceBuf0    = NULL;
STATIC UINT8               *mBounceBuf2    = NULL;

STATIC
UINT32
DwMmcGetSavedClkSel (
  IN UINTN BaseAddr
  )
{
  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    return (mSavedClkSel0 != 0) ? mSavedClkSel0 : 0x03020001;
  }
  return (mSavedClkSel2 != 0) ? mSavedClkSel2 : 0x03020001;
}

STATIC
EFI_STATUS
DwMmcEnsureDmaResources (
  IN  UINTN                 BaseAddr,
  OUT DW_MMC_IDMAC_DESC64 **OutRing,
  OUT UINT8               **OutBounce
  )
{
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  Addr;
  DW_MMC_IDMAC_DESC64 **RingPtr;
  UINT8               **BouncePtr;

  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    RingPtr   = &mIdmacRing0;
    BouncePtr = &mBounceBuf0;
  } else {
    RingPtr   = &mIdmacRing2;
    BouncePtr = &mBounceBuf2;
  }

  if (*RingPtr == NULL) {
    Addr   = 0x8FFFFFFFULL;
    Status = gBS->AllocatePages (
                    AllocateMaxAddress,
                    EfiBootServicesData,
                    EFI_SIZE_TO_PAGES (DW_MMC_IDMAC_RING_COUNT * sizeof (DW_MMC_IDMAC_DESC64)),
                    &Addr
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }
    *RingPtr = (DW_MMC_IDMAC_DESC64 *)(UINTN)Addr;
    ZeroMem (*RingPtr, DW_MMC_IDMAC_RING_COUNT * sizeof (DW_MMC_IDMAC_DESC64));
  }

  if (*BouncePtr == NULL) {
    Addr   = 0x8FFFFFFFULL;
    Status = gBS->AllocatePages (
                    AllocateMaxAddress,
                    EfiBootServicesData,
                    EFI_SIZE_TO_PAGES (DW_MMC_BOUNCE_BUFFER_SIZE),
                    &Addr
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }
    *BouncePtr = (UINT8 *)(UINTN)Addr;
    ZeroMem (*BouncePtr, DW_MMC_BOUNCE_BUFFER_SIZE);
  }

  *OutRing   = *RingPtr;
  *OutBounce = *BouncePtr;
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
  UINT32 Ctrl;

  MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_SWR);
  DwMmcWaitForClear (BaseAddr, DW_MMC_BMOD, DW_MMC_BMOD_SWR, 100000);

  MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_IDINTEN, DW_MMC_IDMAC_INT_MASK);
  MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_DE | DW_MMC_BMOD_FB);

  Ctrl  = MmioRead32 (BaseAddr + DW_MMC_CTRL);
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

  Print (
    L"[DWMMC 0x%x] CMD%u recovery: CMD=0x%x RINT=0x%x STATUS=0x%x CTRL=0x%x CLKSEL=0x%x\n",
    (UINT32)BaseAddr,
    CmdIdx,
    MmioRead32 (BaseAddr + DW_MMC_CMD),
    MmioRead32 (BaseAddr + DW_MMC_RINTSTS),
    MmioRead32 (BaseAddr + DW_MMC_STATUS),
    MmioRead32 (BaseAddr + DW_MMC_CTRL),
    MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL)
    );

  MmioWrite32 (
    BaseAddr + EXYNOS_DWMMC_CLKSEL,
    ClkSelSaved & ~((0x3U << 6) | 0x7U)
    );

  Ctrl  = MmioRead32 (BaseAddr + DW_MMC_CTRL);
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
  UINT32     ClkSelSaved;
  UINT32     Ctrl;
  EFI_STATUS Status;

  ClkSelSaved = MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL);
  if (ClkSelSaved == 0) {
    ClkSelSaved = 0x03020001;
  }

  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    mSavedClkSel0 = ClkSelSaved;
  } else {
    mSavedClkSel2 = ClkSelSaved;
  }

  MmioWrite32 (BaseAddr + DW_MMC_BMOD, 0);
  MmioWrite32 (BaseAddr + DW_MMC_IDINTEN, 0);

  Ctrl  = MmioRead32 (BaseAddr + DW_MMC_CTRL);
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

  MmioWrite32 (BaseAddr + EXYNOS_DWMMC_CLKSEL, ClkSelSaved);
  MmioWrite32 (BaseAddr + DW_MMC_PWREN, 1);
  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_INTMASK, 0);
  MmioWrite32 (BaseAddr + DW_MMC_TMOUT, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_CTYPE, DW_MMC_CTYPE_1BIT);
  MmioWrite32 (BaseAddr + DW_MMC_FIFOTH, (0x2U << 28) | (15U << 16) | 16U);

  DwMmcEnableIdmac (BaseAddr);

  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    Print (
      L"[DW0] VERID=0x%x HCON=0x%x CTRL=0x%x CLKSEL=0x%x\n",
      MmioRead32 (BaseAddr + DW_MMC_VERID),
      MmioRead32 (BaseAddr + DW_MMC_HCON),
      MmioRead32 (BaseAddr + DW_MMC_CTRL),
      MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL)
      );
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
DwMmcPrepareIdmac (
  IN  UINTN                 BaseAddr,
  IN  UINT8                *BounceBuf,
  IN  DW_MMC_IDMAC_DESC64  *Ring,
  IN  UINT32                TotalBytes,
  OUT UINT32               *OutDescCount
  )
{
  UINT32 Offset;
  UINT32 i;
  UINT32 Chunk;
  UINTN  BufAddr;
  UINTN  DescAddr;
  UINT32 Des0;

  if (TotalBytes == 0 || TotalBytes > DW_MMC_BOUNCE_BUFFER_SIZE) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Offset = 0;
  i      = 0;

  while (Offset < TotalBytes) {
    if (i >= DW_MMC_IDMAC_RING_COUNT) {
      return EFI_OUT_OF_RESOURCES;
    }

    Chunk   = TotalBytes - Offset;
    if (Chunk > IDMAC_MAX_BUFFER_SIZE) {
      Chunk = IDMAC_MAX_BUFFER_SIZE;
    }

    BufAddr = (UINTN)(BounceBuf + Offset);

    Des0 = IDMAC_DES0_OWN | IDMAC_DES0_DIC;
    if (i == 0) {
      Des0 |= IDMAC_DES0_FS;
    }

    if ((Offset + Chunk) >= TotalBytes) {
      Des0 &= ~IDMAC_DES0_DIC;
      Des0 |= IDMAC_DES0_LD | IDMAC_DES0_ER;
      Ring[i].NextDescAddrLo = 0;
      Ring[i].NextDescAddrHi = 0;
    } else {
      Des0 |= IDMAC_DES0_CH;
      DescAddr = (UINTN)(&Ring[i + 1]);
      Ring[i].NextDescAddrLo = (UINT32)(DescAddr & 0xFFFFFFFFULL);
      Ring[i].NextDescAddrHi = (UINT32)(DescAddr >> 32);
    }

    Ring[i].Des0          = Des0;
    Ring[i].Des1          = IDMAC_SET_BUFFER1_SIZE (Chunk);
    Ring[i].Buffer1AddrLo = (UINT32)(BufAddr & 0xFFFFFFFFULL);
    Ring[i].Buffer1AddrHi = (UINT32)(BufAddr >> 32);
    Ring[i].Reserved0     = 0;
    Ring[i].Reserved1     = 0;

    Offset += Chunk;
    i++;
  }

  *OutDescCount = i;

  WriteBackInvalidateDataCacheRange (Ring, i * sizeof (DW_MMC_IDMAC_DESC64));
  WriteBackInvalidateDataCacheRange (BounceBuf, TotalBytes);

  DescAddr = (UINTN)Ring;
  MmioWrite32 (BaseAddr + DW_MMC_DBADDRL, (UINT32)(DescAddr & 0xFFFFFFFFULL));
  MmioWrite32 (BaseAddr + DW_MMC_DBADDRU, (UINT32)(DescAddr >> 32));

  return EFI_SUCCESS;
}

EFI_STATUS
DwMmcExecCommand (
  IN     UINTN                               BaseAddr,
  IN OUT EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *Packet
  )
{
  UINT32               CmdReg;
  UINT32               RintSts;
  UINT32               IdSts;
  UINT32               Timeout;
  EFI_STATUS           Status;
  BOOLEAN              IsRead;
  UINT32               TotalBytes;
  UINT8               *UserBuf;
  UINT16               CmdIdx;
  UINT32               CmdArg;
  BOOLEAN              IsDeselect;
  DW_MMC_IDMAC_DESC64 *Ring;
  UINT8               *BounceBuf;
  UINT32               DescCount;

  if (Packet == NULL || Packet->SdMmcCmdBlk == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  CmdReg     = DW_MMC_CMD_START | BIT29;
  IsRead     = FALSE;
  TotalBytes = 0;
  UserBuf    = NULL;
  Ring       = NULL;
  BounceBuf  = NULL;
  DescCount  = 0;

  CmdIdx     = Packet->SdMmcCmdBlk->CommandIndex;
  CmdArg     = Packet->SdMmcCmdBlk->CommandArgument;
  IsDeselect = (CmdIdx == 7 && CmdArg == 0);

  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    mCmdSeq++;
    if (mCmdSeq >= 18 && mCmdSeq <= 32) {
      Print (
        L"[DW0 #%u] CMD%d(0x%x) RespType=%d InLen=%u\n",
        mCmdSeq,
        CmdIdx,
        CmdArg,
        Packet->SdMmcCmdBlk->ResponseType,
        Packet->InTransferLength
        );
    }
  }

  if (CmdIdx != 13 && CmdIdx != 12 && !IsDeselect) {
    CmdReg |= DW_MMC_CMD_WAIT_PRVDATA;
  }

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

  Status = DwMmcWaitForClear (BaseAddr, DW_MMC_CMD, DW_MMC_CMD_START, 500000);
  if (EFI_ERROR (Status)) {
    Print (L"[DWMMC 0x%x] CMD%d TIMEOUT pre-start\n", (UINT32)BaseAddr, CmdIdx);
    DwMmcRecoverController (BaseAddr, CmdIdx);
    Packet->TransactionStatus = EFI_TIMEOUT;
    return EFI_TIMEOUT;
  }

  if (CmdIdx != 13 && CmdIdx != 12 && !IsDeselect) {
    Status = DwMmcWaitForClear (BaseAddr, DW_MMC_STATUS, DW_MMC_STATUS_DATA_BUSY, 500000);
    if (EFI_ERROR (Status)) {
      Print (L"[DWMMC 0x%x] CMD%d TIMEOUT data_busy\n", (UINT32)BaseAddr, CmdIdx);
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
    Status = DwMmcEnsureDmaResources (BaseAddr, &Ring, &BounceBuf);
    if (EFI_ERROR (Status)) {
      Packet->TransactionStatus = Status;
      return Status;
    }

    if (!IsRead) {
      CopyMem (BounceBuf, UserBuf, TotalBytes);
    } else {
      ZeroMem (BounceBuf, TotalBytes);
    }

    MmioOr32 (BaseAddr + DW_MMC_CTRL, DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET);
    Status = DwMmcWaitForClear (
               BaseAddr,
               DW_MMC_CTRL,
               DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET,
               100000
               );
    if (EFI_ERROR (Status)) {
      Packet->TransactionStatus = Status;
      return Status;
    }

    DwMmcEnableIdmac (BaseAddr);

    MmioWrite32 (BaseAddr + DW_MMC_BLKSIZ, (TotalBytes < 512) ? TotalBytes : 512);
    MmioWrite32 (BaseAddr + DW_MMC_BYTCNT, TotalBytes);

    Status = DwMmcPrepareIdmac (BaseAddr, BounceBuf, Ring, TotalBytes, &DescCount);
    if (EFI_ERROR (Status)) {
      Packet->TransactionStatus = Status;
      return Status;
    }

    if (CmdIdx == 18 || CmdIdx == 25) {
      CmdReg |= DW_MMC_CMD_SEND_STOP;
    }
  }

  MmioWrite32 (BaseAddr + DW_MMC_CMDARG, CmdArg);
  MmioWrite32 (BaseAddr + DW_MMC_CMD, CmdReg);

  if (TotalBytes > 0) {
    MmioWrite32 (BaseAddr + DW_MMC_PLDMND, 1);
  }

  Timeout = 500000;
  while (Timeout > 0) {
    RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
    if (RintSts & DW_MMC_INT_ERROR) {
      if (mLastLoggedErr < 8) {
        Print (
          L"[DWMMC 0x%x] CMD%d(0x%x) err RINTSTS=0x%x\n",
          (UINT32)BaseAddr,
          CmdIdx,
          CmdArg,
          RintSts
          );
        mLastLoggedErr++;
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
    Print (L"[DWMMC 0x%x] CMD%d TIMEOUT cmd_done\n", (UINT32)BaseAddr, CmdIdx);
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

  if (TotalBytes > 0 && UserBuf != NULL) {
    Timeout = 2000000;
    while (Timeout > 0) {
      RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
      IdSts   = MmioRead32 (BaseAddr + DW_MMC_IDSTS);

      if ((RintSts & DW_MMC_INT_ERROR) || (IdSts & DW_MMC_IDMAC_INT_FBE)) {
        Print (
          L"[DWMMC 0x%x] CMD%d IDMAC err IDSTS=0x%x RINT=0x%x Desc=0x%x Buf=0x%x\n",
          (UINT32)BaseAddr,
          CmdIdx,
          IdSts,
          RintSts,
          (UINT32)(UINTN)Ring,
          (UINT32)(UINTN)BounceBuf
          );
        MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
        MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
        Packet->TransactionStatus = EFI_DEVICE_ERROR;
        return EFI_DEVICE_ERROR;
      }

      if ((RintSts & DW_MMC_INT_DTO) &&
          (IdSts & (DW_MMC_IDMAC_INT_RI | DW_MMC_IDMAC_INT_TI))) {
        break;
      }

      MicroSecondDelay (1);
      Timeout--;
    }

    if (Timeout == 0) {
      Print (
        L"[DWMMC 0x%x] CMD%d TIMEOUT IDMAC IDSTS=0x%x RINT=0x%x\n",
        (UINT32)BaseAddr,
        CmdIdx,
        MmioRead32 (BaseAddr + DW_MMC_IDSTS),
        MmioRead32 (BaseAddr + DW_MMC_RINTSTS)
        );
      Packet->TransactionStatus = EFI_TIMEOUT;
      return EFI_TIMEOUT;
    }

    MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);

    if (IsRead) {
      InvalidateDataCacheRange (BounceBuf, TotalBytes);
      CopyMem (UserBuf, BounceBuf, TotalBytes);
    }

    if (BaseAddr == EXYNOS_DWMMC0_BASE && CmdIdx == 8 && TotalBytes == 512) {
      UINT32 SecCnt = (UINT32)UserBuf[212] |
                      ((UINT32)UserBuf[213] << 8) |
                      ((UINT32)UserBuf[214] << 16) |
                      ((UINT32)UserBuf[215] << 24);
      Print (
        L"[DW0 CMD8 IDMAC] Rev=%u SecCnt=0x%x B0..7=%02x%02x%02x%02x %02x%02x%02x%02x\n",
        UserBuf[192],
        SecCnt,
        UserBuf[0], UserBuf[1], UserBuf[2], UserBuf[3],
        UserBuf[4], UserBuf[5], UserBuf[6], UserBuf[7]
        );
    }
  }

  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  Packet->TransactionStatus = EFI_SUCCESS;
  return EFI_SUCCESS;
}
