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

#define IDMAC_MAX_DESCS          256
#define IDMAC_BOUNCE_BUF_SIZE    (IDMAC_MAX_DESCS * IDMAC_MAX_BUFFER_SIZE)

STATIC UINT32 mLastLoggedErr = 0;
STATIC UINT32 mCmdSeq        = 0;
STATIC UINT32 mSavedClkSel0  = 0x03020001;
STATIC UINT32 mSavedClkSel2  = 0x03020001;

STATIC DW_MMC_IDMAC_DESC64 *mIdmacRing0   = NULL;
STATIC DW_MMC_IDMAC_DESC64 *mIdmacRing2   = NULL;
STATIC UINT8               *mBounceBuf0   = NULL;
STATIC UINT8               *mBounceBuf2   = NULL;

STATIC
UINT32
GetSavedClkSel (
  IN UINTN BaseAddr
  )
{
  return (BaseAddr == EXYNOS_DWMMC0_BASE) ? mSavedClkSel0 : mSavedClkSel2;
}

STATIC
VOID
SetSavedClkSel (
  IN UINTN  BaseAddr,
  IN UINT32 Val
  )
{
  if (Val == 0) {
    Val = 0x03020001;
  }
  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    mSavedClkSel0 = Val;
  } else {
    mSavedClkSel2 = Val;
  }
}

STATIC
EFI_STATUS
EnsureIdmacResources (
  IN  UINTN                BaseAddr,
  OUT DW_MMC_IDMAC_DESC64  **OutRing,
  OUT UINT8                **OutBounce
  )
{
  EFI_STATUS           Status;
  EFI_PHYSICAL_ADDRESS PhysAddr;
  DW_MMC_IDMAC_DESC64  **RingPtr;
  UINT8                **BufPtr;

  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    RingPtr = &mIdmacRing0;
    BufPtr  = &mBounceBuf0;
  } else {
    RingPtr = &mIdmacRing2;
    BufPtr  = &mBounceBuf2;
  }

  if (*RingPtr == NULL) {
    PhysAddr = 0x87FFFFFFULL;
    Status   = gBS->AllocatePages (
                      AllocateMaxAddress,
                      EfiBootServicesData,
                      EFI_SIZE_TO_PAGES (sizeof (DW_MMC_IDMAC_DESC64) * IDMAC_MAX_DESCS),
                      &PhysAddr
                      );
    if (EFI_ERROR (Status)) {
      return Status;
    }
    *RingPtr = (DW_MMC_IDMAC_DESC64 *)(UINTN)PhysAddr;
    ZeroMem (*RingPtr, sizeof (DW_MMC_IDMAC_DESC64) * IDMAC_MAX_DESCS);
  }

  if (*BufPtr == NULL) {
    PhysAddr = 0x87FFFFFFULL;
    Status   = gBS->AllocatePages (
                      AllocateMaxAddress,
                      EfiBootServicesData,
                      EFI_SIZE_TO_PAGES (IDMAC_BOUNCE_BUF_SIZE),
                      &PhysAddr
                      );
    if (EFI_ERROR (Status)) {
      return Status;
    }
    *BufPtr = (UINT8 *)(UINTN)PhysAddr;
    ZeroMem (*BufPtr, IDMAC_BOUNCE_BUF_SIZE);
  }

  *OutRing   = *RingPtr;
  *OutBounce = *BufPtr;
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

  MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
  MmioWrite32 (
    BaseAddr + DW_MMC_IDINTEN,
    DW_MMC_IDMAC_INT_TI | DW_MMC_IDMAC_INT_RI | DW_MMC_IDMAC_INT_FBE |
    DW_MMC_IDMAC_INT_DU | DW_MMC_IDMAC_INT_CES | DW_MMC_IDMAC_INT_NIS |
    DW_MMC_IDMAC_INT_AIS
    );

  MmioOr32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_SWR);
  DwMmcWaitForClear (BaseAddr, DW_MMC_BMOD, DW_MMC_BMOD_SWR, 10000);

  MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_DE | DW_MMC_BMOD_FB);

  Ctrl = MmioRead32 (BaseAddr + DW_MMC_CTRL);
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

  ClkSelSaved = GetSavedClkSel (BaseAddr);

  Print (
    L"[DWMMC 0x%x] CMD%u recovery: CMD=0x%x RINT=0x%x STATUS=0x%x CTRL=0x%x CLKSEL=0x%x (saved=0x%x)\n",
    (UINT32)BaseAddr,
    CmdIdx,
    MmioRead32 (BaseAddr + DW_MMC_CMD),
    MmioRead32 (BaseAddr + DW_MMC_RINTSTS),
    MmioRead32 (BaseAddr + DW_MMC_STATUS),
    MmioRead32 (BaseAddr + DW_MMC_CTRL),
    MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL),
    ClkSelSaved
    );

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

  Status = DwMmcWaitForClear (BaseAddr, DW_MMC_CMD, DW_MMC_CMD_START, 100000);
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
  SetSavedClkSel (BaseAddr, ClkSelBoot);

  Status = EnsureIdmacResources (BaseAddr, &Ring, &Bounce);
  if (EFI_ERROR (Status)) {
    Print (L"[DWMMC 0x%x] IDMAC alloc failed: 0x%x\n", (UINT32)BaseAddr, (UINT32)Status);
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
             DW_MMC_CTRL_RESET | DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET,
             100000
             );
  if (EFI_ERROR (Status)) {
    Print (L"[DWMMC 0x%x] controller reset timeout\n", (UINT32)BaseAddr);
    return Status;
  }

  MmioWrite32 (BaseAddr + EXYNOS_DWMMC_CLKSEL, GetSavedClkSel (BaseAddr));
  MmioWrite32 (BaseAddr + DW_MMC_PWREN, 1);
  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_INTMASK, 0);
  MmioWrite32 (BaseAddr + DW_MMC_TMOUT, 0xFFFFFFFF);
  MmioWrite32 (BaseAddr + DW_MMC_CTYPE, DW_MMC_CTYPE_1BIT);
  MmioWrite32 (BaseAddr + DW_MMC_FIFOTH, (0x2 << 28) | (15 << 16) | 16);

  DwMmcEnableIdmac (BaseAddr);

  if (BaseAddr == EXYNOS_DWMMC0_BASE) {
    Print (
      L"[DW0] VERID=0x%x HCON=0x%x CTRL=0x%x BMOD=0x%x CLKSEL=0x%x Ring=0x%lx\n",
      MmioRead32 (BaseAddr + DW_MMC_VERID),
      MmioRead32 (BaseAddr + DW_MMC_HCON),
      MmioRead32 (BaseAddr + DW_MMC_CTRL),
      MmioRead32 (BaseAddr + DW_MMC_BMOD),
      MmioRead32 (BaseAddr + EXYNOS_DWMMC_CLKSEL),
      (UINT64)(UINTN)Ring
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
  UINT8               *UserBuf   = NULL;
  UINT16              CmdIdx;
  UINT32              CmdArg;
  BOOLEAN             IsDeselect;
  DW_MMC_IDMAC_DESC64 *Ring      = NULL;
  UINT8               *Bounce    = NULL;
  UINT32              DescCount  = 0;

  if (Packet == NULL || Packet->SdMmcCmdBlk == NULL) {
    return EFI_INVALID_PARAMETER;
  }

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
    Print (L"[DWMMC 0x%x] CMD%d TIMEOUT pre-start\n", (UINT32)BaseAddr, CmdIdx);
    DwMmcRecoverController (BaseAddr, CmdIdx);
    Packet->TransactionStatus = EFI_TIMEOUT;
    return EFI_TIMEOUT;
  }

  if (CmdIdx != 13 && CmdIdx != 12 && !IsDeselect) {
    Status = DwMmcWaitForClear (
               BaseAddr,
               DW_MMC_STATUS,
               DW_MMC_STATUS_DATA_BUSY,
               500000
               );
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
    UINT32 Remaining;
    UINT32 Offset;
    UINT32 i;
    UINT64 DescPhys;
    UINT64 BufPhys;

    if (TotalBytes > IDMAC_BOUNCE_BUF_SIZE) {
      Packet->TransactionStatus = EFI_BAD_BUFFER_SIZE;
      return EFI_BAD_BUFFER_SIZE;
    }

    Status = EnsureIdmacResources (BaseAddr, &Ring, &Bounce);
    if (EFI_ERROR (Status)) {
      Packet->TransactionStatus = Status;
      return Status;
    }

    if (!IsRead) {
      CopyMem (Bounce, UserBuf, TotalBytes);
    } else {
      ZeroMem (Bounce, TotalBytes);
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

    MmioOr32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_SWR);
    DwMmcWaitForClear (BaseAddr, DW_MMC_BMOD, DW_MMC_BMOD_SWR, 10000);
    MmioWrite32 (BaseAddr + DW_MMC_BMOD, DW_MMC_BMOD_DE | DW_MMC_BMOD_FB);
    MmioOr32 (BaseAddr + DW_MMC_CTRL, DW_MMC_CTRL_USE_IDMAC | DW_MMC_CTRL_DMA_ENABLE);

    DescCount = (TotalBytes + IDMAC_MAX_BUFFER_SIZE - 1) / IDMAC_MAX_BUFFER_SIZE;
    Remaining = TotalBytes;
    Offset    = 0;

    for (i = 0; i < DescCount; i++) {
      UINT32 Chunk = (Remaining > IDMAC_MAX_BUFFER_SIZE) ? IDMAC_MAX_BUFFER_SIZE : Remaining;
      UINT32 Flags = IDMAC_DES0_OWN | IDMAC_DES0_DIC;

      if (i == 0) {
        Flags |= IDMAC_DES0_FS;
      }
      if (i == (DescCount - 1)) {
        Flags |= (IDMAC_DES0_LD | IDMAC_DES0_ER);
        Flags &= ~IDMAC_DES0_DIC;
      }

      BufPhys  = (UINT64)(UINTN)(Bounce + Offset);
      DescPhys = (UINT64)(UINTN)(&Ring[i + 1]);

      // Structure idmac_desc_64addr conforme au noyau Linux :
      // Des0 = flags, Des1 = réservé, Des2 = BS1 (taille), Des3 = réservé,
      // Des4/Des5 = adresse buffer 64-bit, Des6/Des7 = descripteur suivant
      Ring[i].Des0           = Flags;
      Ring[i].Des1           = 0;
      Ring[i].Des2           = IDMAC_SET_BUFFER1_SIZE (Chunk);
      Ring[i].Des3           = 0;
      Ring[i].Buffer1AddrLo  = (UINT32)(BufPhys & 0xFFFFFFFFULL);
      Ring[i].Buffer1AddrHi  = (UINT32)(BufPhys >> 32);
      if (i == (DescCount - 1)) {
        Ring[i].NextDescAddrLo = 0;
        Ring[i].NextDescAddrHi = 0;
      } else {
        Ring[i].NextDescAddrLo = (UINT32)(DescPhys & 0xFFFFFFFFULL);
        Ring[i].NextDescAddrHi = (UINT32)(DescPhys >> 32);
      }

      Remaining -= Chunk;
      Offset    += Chunk;
    }

    WriteBackInvalidateDataCacheRange (Ring, sizeof (DW_MMC_IDMAC_DESC64) * DescCount);
    WriteBackInvalidateDataCacheRange (Bounce, TotalBytes);

    DescPhys = (UINT64)(UINTN)Ring;
    MmioWrite32 (BaseAddr + DW_MMC_DBADDRL, (UINT32)(DescPhys & 0xFFFFFFFFULL));
    MmioWrite32 (BaseAddr + DW_MMC_DBADDRU, (UINT32)(DescPhys >> 32));

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
        Print (L"[DWMMC 0x%x] CMD%d(0x%x) err RINTSTS=0x%x\n", (UINT32)BaseAddr, CmdIdx, CmdArg, RintSts);
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

  if (TotalBytes > 0) {
    Timeout = 2000000;
    while (Timeout > 0) {
      RintSts = MmioRead32 (BaseAddr + DW_MMC_RINTSTS);
      IdSts   = MmioRead32 (BaseAddr + DW_MMC_IDSTS);

      if ((IdSts & DW_MMC_IDMAC_INT_FBE) || (RintSts & DW_MMC_INT_ERROR)) {
        Print (
          L"[DWMMC 0x%x] CMD%d IDMAC err IDSTS=0x%x RINT=0x%x Desc=0x%lx Buf=0x%lx DSCADDR=0x%x BUFADDR=0x%x\n",
          (UINT32)BaseAddr,
          CmdIdx,
          IdSts,
          RintSts,
          (UINT64)(UINTN)Ring,
          (UINT64)(UINTN)Bounce,
          MmioRead32 (BaseAddr + DW_MMC_DSCADDRL),
          MmioRead32 (BaseAddr + DW_MMC_BUFADDRL)
          );
        MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
        MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
        Packet->TransactionStatus = EFI_DEVICE_ERROR;
        return EFI_DEVICE_ERROR;
      }

      if ((RintSts & DW_MMC_INT_DTO) ||
          (IdSts & (DW_MMC_IDMAC_INT_RI | DW_MMC_IDMAC_INT_TI))) {
        if (RintSts & DW_MMC_INT_DTO) {
          break;
        }
      }

      MicroSecondDelay (1);
      Timeout--;
    }

    if (Timeout == 0) {
      Print (
        L"[DWMMC 0x%x] CMD%d IDMAC TIMEOUT IDSTS=0x%x RINT=0x%x STATUS=0x%x\n",
        (UINT32)BaseAddr,
        CmdIdx,
        MmioRead32 (BaseAddr + DW_MMC_IDSTS),
        MmioRead32 (BaseAddr + DW_MMC_RINTSTS),
        MmioRead32 (BaseAddr + DW_MMC_STATUS)
        );
      Packet->TransactionStatus = EFI_TIMEOUT;
      return EFI_TIMEOUT;
    }

    if (IsRead) {
      InvalidateDataCacheRange (Bounce, TotalBytes);
      CopyMem (UserBuf, Bounce, TotalBytes);

      if (BaseAddr == EXYNOS_DWMMC0_BASE && CmdIdx == 8 && TotalBytes == 512) {
        UINT32 SecCnt = UserBuf | (UserBuf << 8) |
                          (UserBuf << 16) | (UserBuf << 24);
        Print (
          L"[DW0 CMD8 IDMAC] Rev=%u SecCnt=0x%x B0..7=%02x%02x%02x%02x %02x%02x%02x%02x\n",
          UserBuf,
          SecCnt,
          UserBuf, UserBuf, UserBuf, UserBuf,
          UserBuf, UserBuf, UserBuf, UserBuf
          );
      }
    }

    MmioWrite32 (BaseAddr + DW_MMC_IDSTS, 0xFFFFFFFF);
  }

  MmioWrite32 (BaseAddr + DW_MMC_RINTSTS, 0xFFFFFFFF);
  Packet->TransactionStatus = EFI_SUCCESS;
  return EFI_SUCCESS;
}
