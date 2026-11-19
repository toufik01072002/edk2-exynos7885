#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiLib.h>
#include "DwMmcHci.h"

STATIC UINT32 mLastLoggedErr = 0;

STATIC
EFI_STATUS
DwMmcUpdateClock (
  IN UINTN BaseAddr
  )
{
  UINT32 Timeout = 100000;
  MmioWrite32 (BaseAddr + DW_MMC_CMD, DW_MMC_CMD_START | DW_MMC_CMD_UPD_CLK | DW_MMC_CMD_WAIT_PRVDATA);
  while (Timeout--) {
    if ((MmioRead32 (BaseAddr + DW_MMC_CMD) & DW_MMC_CMD_START) == 0) {
      MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
      return EFI_SUCCESS;
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
  UINT32 ClkSelSaved;
  UINT32 Ctrl;
  UINT32 Timeout = 100000;

  ClkSelSaved = MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL);

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

  MmioWrite32 (BaseAddr + EXYNOS_DWMMC_CLKSEL, ClkSelSaved);

  MmioWrite32 (BaseAddr + DW_MMC_PWREN, 0x1);
  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_INTMASK, 0x0);
  MmioWrite32 (BaseAddr + DW_MMC_TMOUT, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_CTYPE, DW_MMC_CTYPE_1BIT);

  // Seuil FIFO Exynos (profondeur 32 ou 64 mots)
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
    Div = 64;  // Horloge d'identification ~400 kHz
  } else if (ClockFreqKhz <= 25000) {
    Div = 1;   // ~25 MHz
  } else {
    Div = 0;   // Bypass diviseur interne (utilise directement CLKSEL de S-Boot)
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

  if (BusWidth == 4) {
    Ctype = DW_MMC_CTYPE_4BIT;
  } else if (BusWidth == 8) {
    Ctype = DW_MMC_CTYPE_8BIT;
  }

  MmioWrite32 (BaseAddr + DW_MMC_CTYPE, Ctype);
  return EFI_SUCCESS;
}

EFI_STATUS
DwMmcExecCommand (
  IN     UINTN                               BaseAddr,
  IN OUT EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *Packet
  )
{
  UINT32 CmdReg = DW_MMC_CMD_START | DW_MMC_CMD_WAIT_PRVDATA;
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

  // Passer en haute vitesse dès que l'identification (CMD0..CMD3) est terminée
  if (CmdIdx == 7 || CmdIdx == 8 || CmdIdx == 17 || CmdIdx == 18) {
    if (MmioRead32 (BaseAddr + DW_MMC_CLKDIV) == 64) {
      DwMmcSetClock (BaseAddr, 50000);
    }
  }

  Timeout = 500000;
  while ((MmioRead32 (BaseAddr + DW_MMC_STATUS) & DW_MMC_STATUS_DATA_BUSY) && Timeout--) {
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
  } else {
    MmioWrite32 (BaseAddr + DW_MMC_BLKSIZ, 0);
    MmioWrite32 (BaseAddr + DW_MMC_BYTCNT, 0);
  }

  MmioWrite32 (BaseAddr + DW_MMC_CMDARG, Packet->SdMmcCmdBlk->CommandArgument);
  MmioWrite32 (BaseAddr + DW_MMC_CMD, CmdReg);

  Timeout = 500000;
  while (Timeout--) {
    RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
    if (RintSts & DW_MMC_INT_ERROR) {
      if (mLastLoggedErr < 5) {
        Print (L"[DWMMC 0x%x] CMD%d err RINTSTS=0x%x\n", (UINT32)BaseAddr, CmdIdx, RintSts);
        mLastLoggedErr++;
      }
      Packet->TransactionStatus = EFI_DEVICE_ERROR;
      return EFI_DEVICE_ERROR;
    }
    if (RintSts & DW_MMC_INT_CMD_DONE) {
      break;
    }
    MicroSecondDelay (1);
  }
  if (Timeout == 0) {
    if (mLastLoggedErr < 5) {
      Print (L"[DWMMC 0x%x] CMD%d timeout\n", (UINT32)BaseAddr, CmdIdx);
      mLastLoggedErr++;
    }
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
        if (mLastLoggedErr < 5) {
          Print (L"[DWMMC 0x%x] CMD%d data err RINTSTS=0x%x\n", (UINT32)BaseAddr, CmdIdx, RintSts);
          mLastLoggedErr++;
        }
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
  }

  if (Packet->SdMmcCmdBlk->ResponseType == SdMmcResponseTypeR1b) {
    Timeout = 1000000;
    while ((MmioRead32 (BaseAddr + DW_MMC_STATUS) & DW_MMC_STATUS_DATA_BUSY) && Timeout--) {
      MicroSecondDelay (1);
    }
  }

  // Synchroniser CTYPE si EmmcDxe vient d'envoyer CMD6 SWITCH sur BUS_WIDTH (EXT_CSD[183])
  if (CmdIdx == 6 && BaseAddr == EXYNOS_DWMMC0_BASE) {
    UINT32 Arg   = Packet->SdMmcCmdBlk->CommandArgument;
    UINT8  Index = (Arg >> 16) & 0xFF;
    UINT8  Value = (Arg >> 8) & 0xFF;
    if (Index == 183) {
      if (Value == 0) {
        DwMmcSetBusWidth (BaseAddr, 1);
      } else if (Value == 1 || Value == 5) {
        DwMmcSetBusWidth (BaseAddr, 4);
      } else if (Value == 2 || Value == 6) {
        DwMmcSetBusWidth (BaseAddr, 8);
      }
    }
  }

  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  Packet->TransactionStatus = EFI_SUCCESS;
  return EFI_SUCCESS;
}
