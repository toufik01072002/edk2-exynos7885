#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiLib.h>
#include "DwMmcHci.h"

STATIC UINT32 mLastLoggedErr = 0;
STATIC UINT32 mCmdSeq        = 0;

// Sauvegarde statique de CLKSEL (0x09C) par contrôleur pour éviter tout écrasement à 0x0
STATIC UINT32  mSavedClkSel0 = 0x03020001;
STATIC UINT32  mSavedClkSel2 = 0x03020001;
STATIC BOOLEAN mClkSel0Init  = FALSE;
STATIC BOOLEAN mClkSel2Init  = FALSE;

STATIC
UINT32
GetSavedClkSel (
  IN UINTN BaseAddr
  )
{
  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    return mSavedClkSel0;
  }
  return mSavedClkSel2;
}

STATIC
VOID
SaveInitialClkSel (
  IN UINTN BaseAddr
  )
{
  UINT32 Val = MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL);
  if (Val == 0) {
    Val = 0x03020001;
  }

  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    if (!mClkSel0Init) {
      mSavedClkSel0 = Val;
      mClkSel0Init  = TRUE;
    }
  } else {
    if (!mClkSel2Init) {
      mSavedClkSel2 = Val;
      mClkSel2Init  = TRUE;
    }
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

  // Neutraliser temporairement les champs RX : bits 7:6 et 2:0.
  MmioWrite32 (
    BaseAddr + EXYNOS_DWMMC_CLKSEL,
    ClkSelSaved & ~((0x3 << 6) | 0x7)
    );

  // Reset CIU + FIFO en mode PIO.
  Ctrl = MmioRead32 (BaseAddr + DW_MMC_CTRL);
  Ctrl &= ~(DW_MMC_CTRL_USE_IDMAC | DW_MMC_CTRL_DMA_ENABLE | DW_MMC_CTRL_INT_ENABLE);
  Ctrl |= DW_MMC_CTRL_RESET | DW_MMC_CTRL_FIFO_RESET;
  MmioWrite32 (BaseAddr + DW_MMC_CTRL, Ctrl);

  Status = DwMmcWaitForClear (
             BaseAddr,
             DW_MMC_CTRL,
             DW_MMC_CTRL_RESET | DW_MMC_CTRL_FIFO_RESET,
             100000
             );

  // Toujours restaurer la valeur valide de CLKSEL (jamais 0x0).
  MmioWrite32 (BaseAddr + EXYNOS_DWMMC_CLKSEL, ClkSelSaved);

  if (EFI_ERROR (Status)) {
    Print (L"[DWMMC 0x%x] CMD%u recovery: RESET TIMEOUT CTRL=0x%x CLKSEL restored=0x%x\n",
           (UINT32)BaseAddr,
           CmdIdx,
           MmioRead32 (BaseAddr + DW_MMC_CTRL),
           ClkSelSaved);
    return Status;
  }

  // Nettoyer les interruptions résiduelles.
  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_CMDARG, 0);

  // Réinitialiser les registres d'horloge CIU après le reset.
  MmioWrite32 (
    BaseAddr + DW_MMC_CMD,
    DW_MMC_CMD_START | DW_MMC_CMD_UPD_CLK | BIT29
    );

  Timeout = 100000;
  while (Timeout > 0) {
    RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);

    if (RintSts & DW_MMC_INT_HLE) {
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, DW_MMC_INT_HLE);
      Print (L"[DWMMC 0x%x] CMD%u recovery: HLE\n",
             (UINT32)BaseAddr, CmdIdx);
      return EFI_DEVICE_ERROR;
    }

    if ((MmioRead32 (BaseAddr + DW_MMC_CMD) & DW_MMC_CMD_START) == 0) {
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
      Print (L"[DWMMC 0x%x] CMD%u recovery: OK\n",
             (UINT32)BaseAddr, CmdIdx);
      return EFI_SUCCESS;
    }

    MicroSecondDelay (1);
    Timeout--;
  }

  Print (L"[DWMMC 0x%x] CMD%u recovery: UPDATE_CLOCK TIMEOUT CMD=0x%x RINT=0x%x\n",
         (UINT32)BaseAddr,
         CmdIdx,
         MmioRead32 (BaseAddr + DW_MMC_CMD),
         MmioRead32 (BaseAddr + DW_MMC_RINTSTS));

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

  SaveInitialClkSel (BaseAddr);
  ClkSelSaved = GetSavedClkSel (BaseAddr);

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
    Print (L"[DWMMC 0x%x] controller reset timeout\n",
           (UINT32)BaseAddr);
    return Status;
  }

  MmioWrite32 (BaseAddr + DW_MMC_BMOD, 0);
  MmioWrite32 (BaseAddr + DW_MMC_IDINTEN, 0);

  Ctrl = MmioRead32 (BaseAddr + DW_MMC_CTRL);
  Ctrl &= ~(DW_MMC_CTRL_USE_IDMAC |
            DW_MMC_CTRL_DMA_ENABLE |
            DW_MMC_CTRL_INT_ENABLE);
  MmioWrite32 (BaseAddr + DW_MMC_CTRL, Ctrl);

  MmioWrite32 (BaseAddr + EXYNOS_DWMMC_CLKSEL, ClkSelSaved);
  MmioWrite32 (BaseAddr + DW_MMC_PWREN, 1);
  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_INTMASK, 0);
  MmioWrite32 (BaseAddr + DW_MMC_TMOUT, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_CTYPE, DW_MMC_CTYPE_1BIT);
  MmioWrite32 (BaseAddr + DW_MMC_FIFOTH,
               (0x2 << 28) | (15 << 16) | 16);

  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    Print (L"[DW0] VERID=0x%x HCON=0x%x CTRL=0x%x CLKSEL=0x%x\n",
           MmioRead32 (BaseAddr + DW_MMC_VERID),
           MmioRead32 (BaseAddr + DW_MMC_HCON),
           MmioRead32 (BaseAddr + DW_MMC_CTRL),
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

  // Conserver une division modérée (Div=16 au lieu de 4) pour éviter tout décrochage RX
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

EFI_STATUS
DwMmcExecCommand (
  IN     UINTN                               BaseAddr,
  IN OUT EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *Packet
  )
{
  UINT32     CmdReg     = DW_MMC_CMD_START | BIT29;
  UINT32     RintSts;
  UINT32     Timeout;
  EFI_STATUS Status;
  BOOLEAN    IsRead     = FALSE;
  UINT32     TotalBytes = 0;
  UINT8      *Buf8      = NULL;
  UINT16     CmdIdx;
  UINT32     CmdArg;
  BOOLEAN    IsDeselect;
  UINT32     Hcon;
  UINT32     HDataWidth;
  BOOLEAN    IsFifo64;

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

  // Attendre fin de commande précédente
  Status = DwMmcWaitForClear (
             BaseAddr, DW_MMC_CMD, DW_MMC_CMD_START, 500000
             );
  if (EFI_ERROR (Status)) {
    Print (L"[DWMMC 0x%x] CMD%d TIMEOUT pre-start\n",
           (UINT32)BaseAddr, CmdIdx);

    Status = DwMmcRecoverController (BaseAddr, CmdIdx);
    if (EFI_ERROR (Status)) {
      Print (L"[DWMMC 0x%x] CMD%d recovery failed: 0x%x\n",
             (UINT32)BaseAddr, CmdIdx, (UINT32)Status);
    }

    Packet->TransactionStatus = EFI_TIMEOUT;
    return EFI_TIMEOUT;
  }

  // Attendre fin de DATA_BUSY sauf pour CMD12/CMD13/CMD7(0)
  if (CmdIdx != 13 && CmdIdx != 12 && !IsDeselect) {
    Status = DwMmcWaitForClear (
               BaseAddr, DW_MMC_STATUS,
               DW_MMC_STATUS_DATA_BUSY, 500000
               );
    if (EFI_ERROR (Status)) {
      Print (L"[DWMMC 0x%x] CMD%d TIMEOUT data_busy\n", (UINT32)BaseAddr, CmdIdx);
      Packet->TransactionStatus = EFI_TIMEOUT;
      return EFI_TIMEOUT;
    }
  }

  // Passer à une horloge modérée dès CMD7 (Select) ou CMD8
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
    Buf8       = (UINT8 *)Packet->InDataBuffer;
    CmdReg    |= DW_MMC_CMD_DATA_EXP;
  } else if (Packet->OutDataBuffer != NULL && Packet->OutTransferLength > 0) {
    IsRead     = FALSE;
    TotalBytes = Packet->OutTransferLength;
    Buf8       = (UINT8 *)Packet->OutDataBuffer;
    CmdReg    |= (DW_MMC_CMD_DATA_EXP | DW_MMC_CMD_RW);
  }

  if (TotalBytes > 0) {
    MmioOr32 (BaseAddr + DW_MMC_CTRL, DW_MMC_CTRL_FIFO_RESET);
    Status = DwMmcWaitForClear (
               BaseAddr, DW_MMC_CTRL,
               DW_MMC_CTRL_FIFO_RESET, 100000
               );
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
    Print (L"[DWMMC 0x%x] CMD%d TIMEOUT cmd_done\n",
           (UINT32)BaseAddr, CmdIdx);

    Status = DwMmcRecoverController (BaseAddr, CmdIdx);
    if (EFI_ERROR (Status)) {
      Print (L"[DWMMC 0x%x] CMD%d recovery failed: 0x%x\n",
             (UINT32)BaseAddr, CmdIdx, (UINT32)Status);
    }

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

  if (TotalBytes > 0 && Buf8 != NULL) {
    UINT32 BytesDone = 0;

    // Auto-détection de la largeur du bus FIFO via HCON bits [5:3] (HDATA_WIDTH)
    // 0: 16-bit, 1: 32-bit, 2: 64-bit, 3: 128-bit, 4: 256-bit
    Hcon       = MmioRead32 (BaseAddr + DW_MMC_HCON);
    HDataWidth = (Hcon >> 3) & 0x7;
    IsFifo64   = (HDataWidth >= 2);

    Timeout = 2000000;
    while (BytesDone < TotalBytes && Timeout > 0) {
      RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
      if (RintSts & DW_MMC_INT_ERROR) {
        if (mLastLoggedErr < 8) {
          Print (L"[DWMMC 0x%x] CMD%d data err RINTSTS=0x%x\n", (UINT32)BaseAddr, CmdIdx, RintSts);
          mLastLoggedErr++;
        }
        MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
        Packet->TransactionStatus = EFI_DEVICE_ERROR;
        return EFI_DEVICE_ERROR;
      }

      UINT32 FifoCount = (MmioRead32 (BaseAddr + DW_MMC_STATUS) >> DW_MMC_STATUS_FIFO_COUNT_SHIFT) & DW_MMC_STATUS_FIFO_COUNT_MASK;
      if (IsRead) {
        while (FifoCount > 0 && BytesDone < TotalBytes) {
          if (IsFifo64) {
            UINT64 Qword = MmioRead64 (BaseAddr + DW_MMC_FIFO_BASE);
            UINT32 Chunk = (TotalBytes - BytesDone >= 8) ? 8 : (TotalBytes - BytesDone);
            CopyMem (Buf8 + BytesDone, &Qword, Chunk);
            BytesDone += Chunk;
          } else {
            UINT32 Dword = MmioRead32 (BaseAddr + DW_MMC_FIFO_BASE);
            UINT32 Chunk = (TotalBytes - BytesDone >= 4) ? 4 : (TotalBytes - BytesDone);
            CopyMem (Buf8 + BytesDone, &Dword, Chunk);
            BytesDone += Chunk;
          }
          FifoCount--;
        }
      } else {
        while (FifoCount < 32 && BytesDone < TotalBytes) {
          if (IsFifo64) {
            UINT64 Qword = 0;
            UINT32 Chunk = (TotalBytes - BytesDone >= 8) ? 8 : (TotalBytes - BytesDone);
            CopyMem (&Qword, Buf8 + BytesDone, Chunk);
            MmioWrite64 (BaseAddr + DW_MMC_FIFO_BASE, Qword);
            BytesDone += Chunk;
          } else {
            UINT32 Dword = 0;
            UINT32 Chunk = (TotalBytes - BytesDone >= 4) ? 4 : (TotalBytes - BytesDone);
            CopyMem (&Dword, Buf8 + BytesDone, Chunk);
            MmioWrite32 (BaseAddr + DW_MMC_FIFO_BASE, Dword);
            BytesDone += Chunk;
          }
          FifoCount++;
        }
      }

      if (BytesDone < TotalBytes) {
        MicroSecondDelay (1);
      }
      Timeout--;
    }

    if (BytesDone < TotalBytes) {
      Print (L"[DWMMC 0x%x] CMD%d TIMEOUT fifo (%u/%u bytes) HCON=0x%x\n",
             (UINT32)BaseAddr, CmdIdx, BytesDone, TotalBytes, Hcon);
      Packet->TransactionStatus = EFI_TIMEOUT;
      return EFI_TIMEOUT;
    }

    Timeout = 1000000;
    while (Timeout > 0) {
      RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
      if (RintSts & DW_MMC_INT_ERROR) {
        Packet->TransactionStatus = EFI_DEVICE_ERROR;
        MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
        return EFI_DEVICE_ERROR;
      }
      if (RintSts & DW_MMC_INT_DTO) {
        break;
      }
      MicroSecondDelay (1);
      Timeout--;
    }

    // Diagnostic complet de CMD8 (EXT_CSD) sur DWMMC0
    if (BaseAddr == EXYNOS_DWMMC0_BASE && CmdIdx == 8 && TotalBytes == 512) {
      UINT32 SecCount = (UINT32)Buf8[212] |
                        ((UINT32)Buf8[213] << 8) |
                        ((UINT32)Buf8[214] << 16) |
                        ((UINT32)Buf8[215] << 24);
      Print (L"[DW0 CMD8] VERID=0x%x HCON=0x%x (W=%u) Rev=%u SecCnt=0x%x B0..7=%02x%02x%02x%02x %02x%02x%02x%02x\n",
             MmioRead32 (BaseAddr + DW_MMC_VERID),
             Hcon,
             HDataWidth,
             Buf8[192],
             SecCount,
             Buf8[0], Buf8[1], Buf8[2], Buf8[3],
             Buf8[4], Buf8[5], Buf8[6], Buf8[7]);
    }
  }

  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  Packet->TransactionStatus = EFI_SUCCESS;
  return EFI_SUCCESS;
}
