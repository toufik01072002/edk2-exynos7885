#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiLib.h>
#include "DwMmcHci.h"

STATIC UINT32 mLastLoggedErr = 0;

typedef struct {
  UINT16 CmdIdx;
  UINT32 Arg;
  UINT32 Status;
  UINT32 CmdReg;
} CMD_TRACE_ENTRY;

STATIC CMD_TRACE_ENTRY mCmdTrace[6];
STATIC UINTN mCmdTraceIdx = 0;

STATIC
VOID
RecordCmdTrace (
  IN UINT16 CmdIdx,
  IN UINT32 Arg,
  IN UINT32 Status,
  IN UINT32 CmdReg
  )
{
  mCmdTrace[mCmdTraceIdx % 6].CmdIdx = CmdIdx;
  mCmdTrace[mCmdTraceIdx % 6].Arg    = Arg;
  mCmdTrace[mCmdTraceIdx % 6].Status = Status;
  mCmdTrace[mCmdTraceIdx % 6].CmdReg = CmdReg;
  mCmdTraceIdx++;
}

STATIC
VOID
DumpCmdTrace (
  IN UINTN BaseAddr
  )
{
  UINTN Start = (mCmdTraceIdx > 6) ? (mCmdTraceIdx - 6) : 0;
  UINTN i;
  Print (L"--- Trace DWMMC 0x%x (CLKSEL=0x%x, CLKENA=0x%x, CLKDIV=0x%x) ---\n",
         (UINT32)BaseAddr,
         MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL),
         MmioRead32 (BaseAddr + DW_MMC_CLKENA),
         MmioRead32 (BaseAddr + DW_MMC_CLKDIV));
  for (i = Start; i < mCmdTraceIdx; i++) {
    CMD_TRACE_ENTRY *E = &mCmdTrace[i % 6];
    Print (L"  [%d] CMD%d(0x%x) STATUS=0x%x CMD=0x%x\n",
           (UINT32)i, E->CmdIdx, E->Arg, E->Status, E->CmdReg);
  }
}

STATIC
EFI_STATUS
DwMmcUpdateClock (
  IN UINTN BaseAddr
  )
{
  UINT32 Timeout = 100000;
  UINT32 CmdVal  = DW_MMC_CMD_START | DW_MMC_CMD_UPD_CLK | DW_MMC_CMD_WAIT_PRVDATA | BIT29;

  while ((MmioRead32 (BaseAddr + DW_MMC_CMD) & DW_MMC_CMD_START) && Timeout--) {
    MicroSecondDelay (1);
  }

  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_CMDARG, 0);
  MmioWrite32 (BaseAddr + DW_MMC_CMD, CmdVal);

  Timeout = 100000;
  while (Timeout--) {
    if ((MmioRead32 (BaseAddr + DW_MMC_CMD) & DW_MMC_CMD_START) == 0) {
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
      return EFI_SUCCESS;
    }
    if (MmioRead32 (BaseAddr + DW_MMC_RINTSTS) & DW_MMC_INT_HLE) {
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, DW_MMC_INT_HLE);
      MmioWrite32 (BaseAddr + DW_MMC_CMD, CmdVal);
    }
    MicroSecondDelay (1);
  }
  return EFI_TIMEOUT;
}

EFI_STATUS
DwMmcInitController (
  IN UINTN BaseAddr
  )
{
  UINT32 Ctrl;
  UINT32 Timeout = 100000;

  MmioWrite32 (BaseAddr + DW_MMC_BMOD, 0);
  MmioWrite32 (BaseAddr + DW_MMC_IDINTEN, 0);

  Ctrl = MmioRead32 (BaseAddr + DW_MMC_CTRL);
  Ctrl &= ~(DW_MMC_CTRL_USE_IDMAC | DW_MMC_CTRL_INT_ENABLE);
  Ctrl |= (DW_MMC_CTRL_RESET | DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET);
  MmioWrite32 (BaseAddr + DW_MMC_CTRL, Ctrl);

  while (Timeout--) {
    if ((MmioRead32 (BaseAddr + DW_MMC_CTRL) & (DW_MMC_CTRL_RESET | DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET)) == 0) {
      break;
    }
    MicroSecondDelay (1);
  }

  // Configurer CLKSEL en mode SDR standard Exynos 7885 (Sample=1, Drive=2, DivRatio=3)
  // pour écraser les réglages HS400/DDR résiduels du bootloader Samsung
  MmioWrite32 (BaseAddr + EXYNOS_DWMMC_CLKSEL, 0x03020001);

  MmioWrite32 (BaseAddr + DW_MMC_PWREN, 0x1);
  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_INTMASK, 0x0);
  MmioWrite32 (BaseAddr + DW_MMC_TMOUT, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_CTYPE, DW_MMC_CTYPE_1BIT);
  MmioWrite32 (BaseAddr + DW_MMC_FIFOTH, (0x2 << 28) | (15 << 16) | 16);

  return DwMmcSetClock (BaseAddr, 400);
}

EFI_STATUS
DwMmcSetClock (
  IN UINTN  BaseAddr,
  IN UINT32 ClockFreqKhz
  )
{
  UINT32 Div = 0;

  MmioWrite32 (BaseAddr + DW_MMC_CLKENA, 0);
  DwMmcUpdateClock (BaseAddr);

  if (ClockFreqKhz == 0) {
    return EFI_SUCCESS;
  }

  if (ClockFreqKhz <= 400) {
    Div = 64;
  } else if (ClockFreqKhz <= 25000) {
    Div = 4; // ~12.5 MHz SDR ultra-stable sans tuning
  } else {
    Div = 2;
  }

  MmioWrite32 (BaseAddr + DW_MMC_CLKSRC, 0);
  MmioWrite32 (BaseAddr + DW_MMC_CLKDIV, Div);
  DwMmcUpdateClock (BaseAddr);

  MmioWrite32 (BaseAddr + DW_MMC_CLKENA, DW_MMC_CLKENA_CCLK_ENABLE);
  return DwMmcUpdateClock (BaseAddr);
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
  UINT32 CmdReg = DW_MMC_CMD_START | DW_MMC_CMD_WAIT_PRVDATA | BIT29;
  UINT32 RintSts;
  UINT32 Timeout;
  BOOLEAN IsRead = FALSE;
  UINT32 TotalBytes = 0;
  UINT32 *Buf32 = NULL;
  UINT32 WordsDone = 0;
  UINT32 TotalWords = 0;
  UINT16 CmdIdx;

  if (Packet == NULL || Packet->SdMmcCmdBlk == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  CmdIdx = Packet->SdMmcCmdBlk->CommandIndex;

  // Ignorer les bascules BUS_WIDTH (183) et HS_TIMING (185) via CMD6 sur DWMMC0
  // afin de rester en mode SDR 1-bit parfaitement synchronisé avec le contrôleur
  if (CmdIdx == 6 && BaseAddr == EXYNOS_DWMMC0_BASE) {
    UINT32 Arg   = Packet->SdMmcCmdBlk->CommandArgument;
    UINT8  Index = (Arg >> 16) & 0xFF;
    UINT8  Value = (Arg >> 8) & 0xFF;
    if ((Index == 183 || Index == 185) && Value != 0) {
      if (Packet->SdMmcStatusBlk != NULL) {
        Packet->SdMmcStatusBlk->Resp0 = 0x00000900;
      }
      Packet->TransactionStatus = EFI_SUCCESS;
      return EFI_SUCCESS;
    }
  }

  // Passer en fréquence de transfert SDR dès la sélection de carte (CMD7)
  if (CmdIdx == 7 || CmdIdx == 8 || CmdIdx == 17 || CmdIdx == 18) {
    if (MmioRead32 (BaseAddr + DW_MMC_CLKDIV) == 64) {
      DwMmcSetClock (BaseAddr, 25000);
    }
  }

  // Attendre fin de commande précédente et fin de DATA_BUSY
  Timeout = 500000;
  while (((MmioRead32 (BaseAddr + DW_MMC_CMD) & DW_MMC_CMD_START) ||
          (MmioRead32 (BaseAddr + DW_MMC_STATUS) & DW_MMC_STATUS_DATA_BUSY)) && Timeout--) {
    MicroSecondDelay (1);
  }
  if (Timeout == 0) {
    return EFI_TIMEOUT;
  }

  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);

  CmdReg |= (CmdIdx & 0x3F);

  if (CmdIdx == 0) {
    CmdReg |= DW_MMC_CMD_SEND_INIT;
  }

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

  if (Packet->InDataBuffer != NULL && Packet->InTransferLength > 0) {
    IsRead     = TRUE;
    TotalBytes = Packet->InTransferLength;
    Buf32      = (UINT32 *)Packet->InDataBuffer;
    CmdReg    |= DW_MMC_CMD_DATA_EXP;
  } else if (Packet->OutDataBuffer != NULL && Packet->OutTransferLength > 0) {
    IsRead     = FALSE;
    TotalBytes = Packet->OutTransferLength;
    Buf32      = (UINT32 *)Packet->OutDataBuffer;
    CmdReg    |= (DW_MMC_CMD_DATA_EXP | DW_MMC_CMD_RW);
  }

  if (TotalBytes > 0) {
    MmioOr32 (BaseAddr + DW_MMC_CTRL, DW_MMC_CTRL_FIFO_RESET);
    Timeout = 100000;
    while ((MmioRead32 (BaseAddr + DW_MMC_CTRL) & DW_MMC_CTRL_FIFO_RESET) && Timeout--) {
      MicroSecondDelay (1);
    }

    MmioWrite32 (BaseAddr + DW_MMC_BLKSIZ, (TotalBytes < 512) ? TotalBytes : 512);
    MmioWrite32 (BaseAddr + DW_MMC_BYTCNT, TotalBytes);
    TotalWords = (TotalBytes + 3) / 4;

    if (CmdIdx == 18 || CmdIdx == 25) {
      CmdReg |= DW_MMC_CMD_SEND_STOP;
    }
  }

  RecordCmdTrace (
    CmdIdx,
    Packet->SdMmcCmdBlk->CommandArgument,
    MmioRead32 (BaseAddr + DW_MMC_STATUS),
    CmdReg
    );

  MmioWrite32 (BaseAddr + DW_MMC_CMDARG, Packet->SdMmcCmdBlk->CommandArgument);
  MmioWrite32 (BaseAddr + DW_MMC_CMD, CmdReg);

  Timeout = 500000;
  while (Timeout--) {
    RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
    if (RintSts & DW_MMC_INT_ERROR) {
      if (mLastLoggedErr < 4) {
        Print (L"[DWMMC 0x%x] CMD%d(0x%x) err RINTSTS=0x%x\n",
               (UINT32)BaseAddr, CmdIdx, Packet->SdMmcCmdBlk->CommandArgument, RintSts);
        DumpCmdTrace (BaseAddr);
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
  }
  if (Timeout == 0) {
    Packet->TransactionStatus = EFI_TIMEOUT;
    return EFI_TIMEOUT;
  }

  if (Packet->SdMmcStatusBlk != NULL) {
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

  if (TotalWords > 0 && Buf32 != NULL) {
    Timeout = 2000000;
    while (WordsDone < TotalWords && Timeout--) {
      RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
      if (RintSts & DW_MMC_INT_ERROR) {
        if (mLastLoggedErr < 4) {
          Print (L"[DWMMC 0x%x] CMD%d data err RINTSTS=0x%x\n", (UINT32)BaseAddr, CmdIdx, RintSts);
          DumpCmdTrace (BaseAddr);
          mLastLoggedErr++;
        }
        MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
        Packet->TransactionStatus = EFI_DEVICE_ERROR;
        return EFI_DEVICE_ERROR;
      }

      UINT32 FifoCount = (MmioRead32 (BaseAddr + DW_MMC_STATUS) >> DW_MMC_STATUS_FIFO_COUNT_SHIFT) & DW_MMC_STATUS_FIFO_COUNT_MASK;
      if (IsRead) {
        while (FifoCount > 0 && WordsDone < TotalWords) {
          Buf32[WordsDone++] = MmioRead32 (BaseAddr + DW_MMC_FIFO_BASE);
          FifoCount--;
        }
      } else {
        while (FifoCount < 32 && WordsDone < TotalWords) {
          MmioWrite32 (BaseAddr + DW_MMC_FIFO_BASE, Buf32[WordsDone++]);
          FifoCount++;
        }
      }
      if (WordsDone < TotalWords) {
        MicroSecondDelay (1);
      }
    }

    if (WordsDone < TotalWords) {
      Packet->TransactionStatus = EFI_TIMEOUT;
      return EFI_TIMEOUT;
    }

    Timeout = 1000000;
    while (Timeout--) {
      RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
      if (RintSts & DW_MMC_INT_DTO) {
        break;
      }
      MicroSecondDelay (1);
    }

    // Reset FIFO matériel post-transfert pour éliminer tout bit RXDR/TXDR résiduel
    MmioOr32 (BaseAddr + DW_MMC_CTRL, DW_MMC_CTRL_FIFO_RESET);
    Timeout = 100000;
    while ((MmioRead32 (BaseAddr + DW_MMC_CTRL) & DW_MMC_CTRL_FIFO_RESET) && Timeout--) {
      MicroSecondDelay (1);
    }
  }

  if (Packet->SdMmcCmdBlk->ResponseType == SdMmcResponseTypeR1b) {
    Timeout = 1000000;
    while ((MmioRead32 (BaseAddr + DW_MMC_STATUS) & DW_MMC_STATUS_DATA_BUSY) && Timeout--) {
      MicroSecondDelay (1);
    }
  }

  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  Packet->TransactionStatus = EFI_SUCCESS;
  return EFI_SUCCESS;
}
