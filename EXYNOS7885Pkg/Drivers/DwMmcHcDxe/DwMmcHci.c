#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiLib.h>
#include "DwMmcHci.h"

STATIC UINT32 mLastLoggedErr = 0;
STATIC UINT32 mCmdSeq        = 0;
STATIC UINT32 mSavedClkSel0  = 0x03020001;
STATIC UINT32 mSavedClkSel2  = 0x03020001;

STATIC DW_MMC_IDMAC_DESC64 *mIdmacRing0 = NULL;
STATIC DW_MMC_IDMAC_DESC64 *mIdmacRing2 = NULL;

STATIC
UINT32
GetSavedClkSel (
  IN UINTN BaseAddr
  )
{
  return (BaseAddr == EXYNOS_DWMMC0_BASE) ? mSavedClkSel0 : mSavedClkSel2;
}

STATIC
DW_MMC_IDMAC_DESC64 *
GetIdmacRing (
  IN UINTN BaseAddr
  )
{
  DW_MMC_IDMAC_DESC64 **RingPtr = (BaseAddr == EXYNOS_DWMMC0_BASE) ? &mIdmacRing0 : &mIdmacRing2;
  if (*RingPtr == NULL) {
    *RingPtr = AllocateAlignedPages (
                 EFI_SIZE_TO_PAGES (sizeof (DW_MMC_IDMAC_DESC64) * DW_MMC_IDMAC_MAX_DESC),
                 64
                 );
    if (*RingPtr != NULL) {
      ZeroMem (*RingPtr, sizeof (DW_MMC_IDMAC_DESC64) * DW_MMC_IDMAC_MAX_DESC);
      WriteBackInvalidateDataCacheRange (*RingPtr, sizeof (DW_MMC_IDMAC_DESC64) * DW_MMC_IDMAC_MAX_DESC);
    }
  }
  return *RingPtr;
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
  IN UINTN               BaseAddr,
  IN DW_MMC_IDMAC_DESC64 *Ring
  )
{
  UINTN  RingPhys = (UINTN)Ring;
  UINT32 Ctrl;

  MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_SWR);
  DwMmcWaitForClear (BaseAddr, DW_MMC_BMOD, DW_MMC_BMOD_SWR, 10000);

  MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_IDINTEN,
               DW_MMC_IDMAC_INT_TI | DW_MMC_IDMAC_INT_RI |
               DW_MMC_IDMAC_INT_FBE | DW_MMC_IDMAC_INT_DU |
               DW_MMC_IDMAC_INT_CES | DW_MMC_IDMAC_INT_NIS |
               DW_MMC_IDMAC_INT_AIS);

  MmioWrite32 (BaseAddr + DW_MMC_DBADDRL, (UINT32)(RingPhys & 0xFFFFFFFFULL));
  MmioWrite32 (BaseAddr + DW_MMC_DBADDRU, (UINT32)((UINT64)RingPhys >> 32));

  Ctrl = MmioRead32 (BaseAddr + DW_MMC_CTRL);
  Ctrl |= (DW_MMC_CTRL_USE_IDMAC | DW_MMC_CTRL_DMA_ENABLE);
  MmioWrite32 (BaseAddr + DW_MMC_CTRL, Ctrl);

  MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_DE | DW_MMC_BMOD_FB);
}

STATIC
EFI_STATUS
DwMmcSetupIdmacDescriptors (
  IN UINTN               BaseAddr,
  IN DW_MMC_IDMAC_DESC64 *Ring,
  IN VOID                *DataBuffer,
  IN UINT32              TotalBytes
  )
{
  UINTN  BufAddr   = (UINTN)DataBuffer;
  UINT32 Remaining = TotalBytes;
  UINT32 Idx       = 0;

  while (Remaining > 0) {
    UINT32 Chunk = (Remaining > DW_MMC_IDMAC_MAX_BYTES_PER_DESC)
                   ? DW_MMC_IDMAC_MAX_BYTES_PER_DESC
                   : Remaining;
    UINT32 Des0  = IDMAC_DES0_OWN | IDMAC_DES0_CH;

    if (Idx >= DW_MMC_IDMAC_MAX_DESC) {
      return EFI_OUT_OF_RESOURCES;
    }

    if (Idx == 0) {
      Des0 |= IDMAC_DES0_FS;
    }

    Remaining -= Chunk;
    if (Remaining == 0) {
      Des0 |= IDMAC_DES0_LD;
      Ring[Idx].Des4 = 0;
      Ring[Idx].Des5 = 0;
    } else {
      UINTN NextDesc = (UINTN)&Ring[Idx + 1];
      Des0 |= IDMAC_DES0_DIC;
      Ring[Idx].Des4 = (UINT32)(NextDesc & 0xFFFFFFFFULL);
      Ring[Idx].Des5 = (UINT32)((UINT64)NextDesc >> 32);
    }

    Ring[Idx].Des0 = Des0;
    Ring[Idx].Des1 = Chunk;
    Ring[Idx].Des2 = (UINT32)(BufAddr & 0xFFFFFFFFULL);
    Ring[Idx].Des3 = (UINT32)((UINT64)BufAddr >> 32);
    Ring[Idx].Des6 = 0;
    Ring[Idx].Des7 = 0;

    BufAddr += Chunk;
    Idx++;
  }

  WriteBackInvalidateDataCacheRange (Ring, sizeof (DW_MMC_IDMAC_DESC64) * Idx);
  DwMmcEnableIdmac (BaseAddr, Ring);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
DwMmcRecoverController (
  IN UINTN  BaseAddr,
  IN UINT32 CmdIdx
  )
{
  EFI_STATUS          Status;
  UINT32              Timeout;
  UINT32              RintSts;
  UINT32              Ctrl;
  UINT32              ClkSelSaved;
  DW_MMC_IDMAC_DESC64 *Ring;

  ClkSelSaved = GetSavedClkSel (BaseAddr);

  Print (L"[DWMMC 0x%x] CMD%u recovery: CMD=0x%x RINT=0x%x STATUS=0x%x CTRL=0x%x CLKENA=0x%x CLKDIV=0x%x CLKSEL=0x%x (saved=0x%x)\n",
         (UINT32)BaseAddr,
         CmdIdx,
         MmioRead32 (BaseAddr + DW_MMC_CMD),
         MmioRead32 (BaseAddr + DW_MMC_RINTSTS),
         MmioRead32 (BaseAddr + DW_MMC_STATUS),
         MmioRead32 (BaseAddr + DW_MMC_CTRL),
         MmioRead32 (BaseAddr + DW_MMC_CLKENA),
         MmioRead32 (BaseAddr + DW_MMC_CLKDIV),
         MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL),
         ClkSelSaved);

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
    Print (L"[DWMMC 0x%x] CMD%u recovery: RESET TIMEOUT CTRL=0x%x\n",
           (UINT32)BaseAddr,
           CmdIdx,
           MmioRead32 (BaseAddr + DW_MMC_CTRL));
    return Status;
  }

  Ring = GetIdmacRing (BaseAddr);
  if (Ring != NULL) {
    DwMmcEnableIdmac (BaseAddr, Ring);
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
      Print (L"[DWMMC 0x%x] CMD%u recovery: OK\n", (UINT32)BaseAddr, CmdIdx);
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
  UINT32              ClkSelSaved;
  UINT32              Ctrl;
  EFI_STATUS          Status;
  DW_MMC_IDMAC_DESC64 *Ring;

  ClkSelSaved = MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL);
  if (ClkSelSaved == 0) {
    ClkSelSaved = 0x03020001;
  }

  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    mSavedClkSel0 = ClkSelSaved;
  } else {
    mSavedClkSel2 = ClkSelSaved;
  }

  Ctrl = MmioRead32 (BaseAddr + DW_MMC_CTRL);
  Ctrl &= ~DW_MMC_CTRL_INT_ENABLE;
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
  MmioWrite32 (BaseAddr + DW_MMC_FIFOTH, (0x2 << 28) | (15 << 16) | 16);

  Ring = GetIdmacRing (BaseAddr);
  if (Ring != NULL) {
    DwMmcEnableIdmac (BaseAddr, Ring);
  }

  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    Print (L"[DW0 IDMAC] VERID=0x%x HCON=0x%x CTRL=0x%x CLKSEL=0x%x RING=0x%lx\n",
           MmioRead32 (BaseAddr + DW_MMC_VERID),
           MmioRead32 (BaseAddr + DW_MMC_HCON),
           MmioRead32 (BaseAddr + DW_MMC_CTRL),
           MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL),
           (UINT64)(UINTN)Ring);
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

EFI_STATUS
DwMmcExecCommand (
  IN     UINTN                               BaseAddr,
  IN OUT EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *Packet
  )
{
  UINT32              CmdReg = DW_MMC_CMD_START | BIT29;
  UINT32              RintSts;
  UINT32              IdSts;
  UINT32              Timeout;
  EFI_STATUS          Status;
  BOOLEAN             IsRead     = FALSE;
  UINT32              TotalBytes = 0;
  VOID                *DataBuf   = NULL;
  UINT16              CmdIdx;
  UINT32              CmdArg;
  BOOLEAN             IsDeselect;
  DW_MMC_IDMAC_DESC64 *Ring;

  if (Packet == NULL || Packet->SdMmcCmdBlk == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  CmdIdx     = Packet->SdMmcCmdBlk->CommandIndex;
  CmdArg     = Packet->SdMmcCmdBlk->CommandArgument;
  IsDeselect = (CmdIdx == 7 && CmdArg == 0);

  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    mCmdSeq++;
    if (mCmdSeq >= 18 && mCmdSeq <= 32) {
      Print (L"[DW0 #%u] CMD%d(0x%x) RespType=%d InLen=%u\n",
             mCmdSeq, CmdIdx, CmdArg, Packet->SdMmcCmdBlk->ResponseType, Packet->InTransferLength);
    }
  }

  if (CmdIdx != 13 && CmdIdx != 12 && !IsDeselect) {
    CmdReg |= DW_MMC_CMD_WAIT_PRVDATA;
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

  Status = DwMmcWaitForClear (BaseAddr, DW_MMC_CMD, DW_MMC_CMD_START, 500000);
  if (EFI_ERROR (Status)) {
    Status = DwMmcRecoverController (BaseAddr, CmdIdx);
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

  MmioWrite32 (BaseAddr + EXYNOS_DWMMC_CLKSEL, GetSavedClkSel (BaseAddr));
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
    DataBuf    = Packet->InDataBuffer;
    CmdReg    |= DW_MMC_CMD_DATA_EXP;
  } else if (Packet->OutDataBuffer != NULL && Packet->OutTransferLength > 0) {
    IsRead     = FALSE;
    TotalBytes = Packet->OutTransferLength;
    DataBuf    = Packet->OutDataBuffer;
    CmdReg    |= (DW_MMC_CMD_DATA_EXP | DW_MMC_CMD_RW);
  }

  if (TotalBytes > 0 && DataBuf != NULL) {
    Ring = GetIdmacRing (BaseAddr);
    if (Ring == NULL) {
      Packet->TransactionStatus = EFI_OUT_OF_RESOURCES;
      return EFI_OUT_OF_RESOURCES;
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

    WriteBackInvalidateDataCacheRange (DataBuf, TotalBytes);

    Status = DwMmcSetupIdmacDescriptors (BaseAddr, Ring, DataBuf, TotalBytes);
    if (EFI_ERROR (Status)) {
      Packet->TransactionStatus = Status;
      return Status;
    }

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

  Timeout = 500000;
  while (Timeout > 0) {
    RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
    if (RintSts & DW_MMC_INT_ERROR) {
      if (mLastLoggedErr < 8) {
        Print (L"[DWMMC 0x%x] CMD%d(0x%x) err RINTSTS=0x%x\n",
               (UINT32)BaseAddr, CmdIdx, CmdArg, RintSts);
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

  if (TotalBytes > 0 && DataBuf != NULL) {
    BOOLEAN DtoDone   = FALSE;
    BOOLEAN IdmacDone = FALSE;

    Timeout = 2000000;
    while (Timeout > 0) {
      RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
      IdSts   = MmioRead32 (BaseAddr + DW_MMC_IDSTS);

      if (RintSts & DW_MMC_INT_ERROR) {
        Print (L"[DWMMC 0x%x] CMD%d data err RINT=0x%x IDSTS=0x%x\n",
               (UINT32)BaseAddr, CmdIdx, RintSts, IdSts);
        MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
        MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
        Packet->TransactionStatus = EFI_DEVICE_ERROR;
        return EFI_DEVICE_ERROR;
      }

      if (IdSts & (DW_MMC_IDMAC_INT_FBE | DW_MMC_IDMAC_INT_DU | DW_MMC_IDMAC_INT_CES)) {
        Print (L"[DWMMC 0x%x] CMD%d IDMAC err IDSTS=0x%x RINT=0x%x\n",
               (UINT32)BaseAddr, CmdIdx, IdSts, RintSts);
        MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
        MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
        Packet->TransactionStatus = EFI_DEVICE_ERROR;
        return EFI_DEVICE_ERROR;
      }

      if (RintSts & DW_MMC_INT_DTO) {
        DtoDone = TRUE;
      }
      if (IdSts & (IsRead ? DW_MMC_IDMAC_INT_RI : DW_MMC_IDMAC_INT_TI)) {
        IdmacDone = TRUE;
      }

      if (DtoDone && IdmacDone) {
        break;
      }

      MicroSecondDelay (1);
      Timeout--;
    }

    if (!DtoDone || !IdmacDone) {
      Print (L"[DWMMC 0x%x] CMD%d TIMEOUT IDMAC (DTO=%d DMA=%d RINT=0x%x IDSTS=0x%x STATUS=0x%x)\n",
             (UINT32)BaseAddr, CmdIdx, DtoDone, IdmacDone,
             MmioRead32 (BaseAddr + DW_MMC_RINTSTS),
             MmioRead32 (BaseAddr + DW_MMC_IDSTS),
             MmioRead32 (BaseAddr + DW_MMC_STATUS));
      DwMmcRecoverController (BaseAddr, CmdIdx);
      Packet->TransactionStatus = EFI_TIMEOUT;
      return EFI_TIMEOUT;
    }

    if (IsRead) {
      InvalidateDataCacheRange (DataBuf, TotalBytes);
    }

    if (BaseAddr == EXYNOS_DWMMC0_BASE && CmdIdx == 8 && TotalBytes == 512) {
      UINT8  *B       = (UINT8 *)DataBuf;
      UINT32 SecCount = B[212] | ((UINT32)B[213] << 8) | ((UINT32)B[214] << 16) | ((UINT32)B[215] << 24);
      Print (L"[DW0 CMD8 IDMAC] Rev=%u SecCnt=0x%x B0..7=%02x%02x%02x%02x %02x%02x%02x%02x\n",
             B[192], SecCount,
             B[0], B[1], B[2], B[3], B[4], B[5], B[6], B[7]);
    }
  }

  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
  Packet->TransactionStatus = EFI_SUCCESS;
  return EFI_SUCCESS;
}
