#ifndef _DW_MMC_HCI_H_
#define _DW_MMC_HCI_H_

#include <Uefi.h>
#include <Protocol/SdMmcPassThru.h>

// Contrôleurs Exynos 7885 confirmés via /proc/iomem
#define EXYNOS_DWMMC0_BASE        0x13500000  // eMMC interne (Slot 0)
#define EXYNOS_DWMMC2_BASE        0x13550000  // microSD (Slot 1)
#define EXYNOS_DWMMC_MAX_SLOTS    2

// Offsets registres DesignWare MMC
#define DW_MMC_CTRL               0x000
#define DW_MMC_PWREN              0x004
#define DW_MMC_CLKDIV             0x008
#define DW_MMC_CLKSRC             0x00C
#define DW_MMC_CLKENA             0x010
#define DW_MMC_TMOUT              0x014
#define DW_MMC_CTYPE              0x018
#define DW_MMC_BLKSIZ             0x01C
#define DW_MMC_BYTCNT             0x020
#define DW_MMC_INTMASK            0x024
#define DW_MMC_CMDARG             0x028
#define DW_MMC_CMD                0x02C
#define DW_MMC_RESP0              0x030
#define DW_MMC_RESP1              0x034
#define DW_MMC_RESP2              0x038
#define DW_MMC_RESP3              0x03C
#define DW_MMC_MINTSTS            0x040
#define DW_MMC_RINTSTS            0x044
#define DW_MMC_STATUS             0x048
#define DW_MMC_FIFOTH             0x04C
#define DW_MMC_CDETECT            0x050
#define DW_MMC_VERID              0x06C
#define DW_MMC_HCON               0x070
#define DW_MMC_UHS_REG            0x074
#define DW_MMC_BMOD               0x080
#define DW_MMC_IDSTS              0x08C
#define DW_MMC_IDINTEN            0x090
#define EXYNOS_DWMMC_CLKSEL       0x09C  // Registre propriétaire Samsung Exynos
#define DW_MMC_FIFO_BASE          0x200  // FIFO Exynos 7885 (v2.70a+)

// Bits CTRL
#define DW_MMC_CTRL_RESET         BIT0
#define DW_MMC_CTRL_FIFO_RESET    BIT1
#define DW_MMC_CTRL_DMA_RESET     BIT2
#define DW_MMC_CTRL_INT_ENABLE    BIT4
#define DW_MMC_CTRL_DMA_ENABLE    BIT5
#define DW_MMC_CTRL_USE_IDMAC     BIT25

// Bits CLKENA
#define DW_MMC_CLKENA_CCLK_ENABLE BIT0
#define DW_MMC_CLKENA_CCLK_LOW_PWR BIT16

// Bits CTYPE
#define DW_MMC_CTYPE_1BIT         0x0
#define DW_MMC_CTYPE_4BIT         BIT0
#define DW_MMC_CTYPE_8BIT         BIT16

// Bits CMD
#define DW_MMC_CMD_RESP_EXP       BIT6
#define DW_MMC_CMD_RESP_LENGTH    BIT7
#define DW_MMC_CMD_CHECK_RESP_CRC BIT8
#define DW_MMC_CMD_DATA_EXP       BIT9
#define DW_MMC_CMD_RW             BIT10
#define DW_MMC_CMD_SEND_STOP      BIT12
#define DW_MMC_CMD_WAIT_PRVDATA   BIT13
#define DW_MMC_CMD_SEND_INIT      BIT15
#define DW_MMC_CMD_UPD_CLK        BIT21
#define DW_MMC_CMD_START          BIT31

// Bits RINTSTS
#define DW_MMC_INT_CD             BIT0
#define DW_MMC_INT_RE             BIT1
#define DW_MMC_INT_CMD_DONE       BIT2
#define DW_MMC_INT_DTO            BIT3
#define DW_MMC_INT_TXDR           BIT4
#define DW_MMC_INT_RXDR           BIT5
#define DW_MMC_INT_RCRC           BIT6
#define DW_MMC_INT_DCRC           BIT7
#define DW_MMC_INT_RTO            BIT8
#define DW_MMC_INT_DRTO           BIT9
#define DW_MMC_INT_HTO            BIT10
#define DW_MMC_INT_FRUN           BIT11
#define DW_MMC_INT_HLE            BIT12
#define DW_MMC_INT_SBE            BIT13
#define DW_MMC_INT_ACD            BIT14
#define DW_MMC_INT_EBE            BIT15
#define DW_MMC_INT_ERROR          (DW_MMC_INT_RE | DW_MMC_INT_RCRC | DW_MMC_INT_DCRC | \
                                   DW_MMC_INT_RTO | DW_MMC_INT_DRTO | DW_MMC_INT_HTO | \
                                   DW_MMC_INT_FRUN | DW_MMC_INT_HLE | DW_MMC_INT_SBE | \
                                   DW_MMC_INT_EBE)

// Bits STATUS
#define DW_MMC_STATUS_FIFO_EMPTY  BIT2
#define DW_MMC_STATUS_FIFO_FULL   BIT3
#define DW_MMC_STATUS_DATA_BUSY   BIT9
#define DW_MMC_STATUS_FIFO_COUNT_MASK  0x1FFF
#define DW_MMC_STATUS_FIFO_COUNT_SHIFT 17

EFI_STATUS
DwMmcInitController (
  IN UINTN BaseAddr
  );

EFI_STATUS
DwMmcExecCommand (
  IN     UINTN                               BaseAddr,
  IN OUT EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *Packet
  );

EFI_STATUS
DwMmcSetClock (
  IN UINTN  BaseAddr,
  IN UINT32 ClockFreqKhz
  );

EFI_STATUS
DwMmcSetBusWidth (
  IN UINTN BaseAddr,
  IN UINT8 BusWidth
  );

#endif
