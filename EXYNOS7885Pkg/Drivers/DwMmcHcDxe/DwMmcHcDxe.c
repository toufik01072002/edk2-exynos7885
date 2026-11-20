#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "DwMmcHcDxe.h"

#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH       Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} DW_MMC_DEVICE_PATH;
#pragma pack()

STATIC DW_MMC_DEVICE_PATH mDwMmcDevicePath = {
  {
    { HARDWARE_DEVICE_PATH, HW_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    { 0x8B4E3C12, 0x9F2A, 0x4D7E, { 0xB1, 0x05, 0x7A, 0x8C, 0x9E, 0x2D, 0x4F, 0x11 } }
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC
EFI_STATUS
EFIAPI
DwMmcPassThruPassThru (
  IN     EFI_SD_MMC_PASS_THRU_PROTOCOL       *This,
  IN     UINT8                               Slot,
  IN OUT EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *Packet,
  IN     EFI_EVENT                           Event OPTIONAL
  )
{
  DW_MMC_HC_PRIVATE_DATA *Private;
  EFI_STATUS             Status;

  if (This == NULL || Packet == NULL || Slot >= EXYNOS_DWMMC_MAX_SLOTS) {
    return EFI_INVALID_PARAMETER;
  }

  Private = DW_MMC_HC_PRIVATE_FROM_THIS (This);
  if (Private->Signature != DW_MMC_HC_PRIVATE_SIGNATURE) {
    return EFI_INVALID_PARAMETER;
  }

  if (!Private->SlotInitialized[Slot]) {
    Packet->TransactionStatus = EFI_NO_MEDIA;
    Status = EFI_NO_MEDIA;
  } else {
    Status = DwMmcExecCommand (Private->SlotBase[Slot], Packet);
  }

  if (Event != NULL) {
    gBS->SignalEvent (Event);
  }
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
DwMmcPassThruGetNextSlot (
  IN     EFI_SD_MMC_PASS_THRU_PROTOCOL *This,
  IN OUT UINT8                         *Slot
  )
{
  DW_MMC_HC_PRIVATE_DATA *Private;

  if (This == NULL || Slot == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Private = DW_MMC_HC_PRIVATE_FROM_THIS (This);
  if (Private->Signature != DW_MMC_HC_PRIVATE_SIGNATURE) {
    return EFI_INVALID_PARAMETER;
  }

  if (*Slot == 0xFF) {
    if (Private->SlotInitialized[0]) {
      *Slot = 0;
      return EFI_SUCCESS;
    }

    if (Private->SlotInitialized[1]) {
      *Slot = 1;
      return EFI_SUCCESS;
    }

    return EFI_NOT_FOUND;
  }

  if (*Slot == 0 && Private->SlotInitialized[1] &&
      ((MmioRead32 (Private->SlotBase[1] + DW_MMC_CDETECT) & BIT0) == 0)) {
    *Slot = 1;
    return EFI_SUCCESS;
  }

  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
EFIAPI
DwMmcPassThruBuildDevicePath (
  IN     EFI_SD_MMC_PASS_THRU_PROTOCOL *This,
  IN     UINT8                         Slot,
  OUT    EFI_DEVICE_PATH_PROTOCOL      **DevicePath
  )
{
  DW_MMC_HC_PRIVATE_DATA *Private;
  SD_DEVICE_PATH         *Node;

  if (This == NULL || DevicePath == NULL || Slot >= EXYNOS_DWMMC_MAX_SLOTS) {
    return EFI_INVALID_PARAMETER;
  }

  Private = DW_MMC_HC_PRIVATE_FROM_THIS (This);
  if (Private->Signature != DW_MMC_HC_PRIVATE_SIGNATURE) {
    return EFI_INVALID_PARAMETER;
  }

  if (!Private->SlotInitialized[Slot]) {
    return EFI_NO_MEDIA;
  }

  if (Slot == 0) {
    EMMC_DEVICE_PATH *EmmcNode = AllocateZeroPool (sizeof (EMMC_DEVICE_PATH));
    if (EmmcNode == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    EmmcNode->Header.Type      = MESSAGING_DEVICE_PATH;
    EmmcNode->Header.SubType   = MSG_EMMC_DP;
    SetDevicePathNodeLength (&EmmcNode->Header, sizeof (EMMC_DEVICE_PATH));
    EmmcNode->SlotNumber       = Slot;
    *DevicePath                = (EFI_DEVICE_PATH_PROTOCOL *)EmmcNode;
  } else {
    Node = AllocateZeroPool (sizeof (SD_DEVICE_PATH));
    if (Node == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }
    Node->Header.Type      = MESSAGING_DEVICE_PATH;
    Node->Header.SubType   = MSG_SD_DP;
    SetDevicePathNodeLength (&Node->Header, sizeof (SD_DEVICE_PATH));
    Node->SlotNumber       = Slot;
    *DevicePath            = (EFI_DEVICE_PATH_PROTOCOL *)Node;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DwMmcPassThruGetSlotNumber (
  IN  EFI_SD_MMC_PASS_THRU_PROTOCOL *This,
  IN  EFI_DEVICE_PATH_PROTOCOL      *DevicePath,
  OUT UINT8                         *Slot
  )
{
  if (This == NULL || DevicePath == NULL || Slot == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (DevicePath->Type == MESSAGING_DEVICE_PATH && DevicePath->SubType == MSG_EMMC_DP) {
    *Slot = ((EMMC_DEVICE_PATH *)DevicePath)->SlotNumber;
    return EFI_SUCCESS;
  }
  if (DevicePath->Type == MESSAGING_DEVICE_PATH && DevicePath->SubType == MSG_SD_DP) {
    *Slot = ((SD_DEVICE_PATH *)DevicePath)->SlotNumber;
    return EFI_SUCCESS;
  }

  return EFI_UNSUPPORTED;
}

STATIC
EFI_STATUS
EFIAPI
DwMmcPassThruResetDevice (
  IN EFI_SD_MMC_PASS_THRU_PROTOCOL *This,
  IN UINT8                         Slot
  )
{
  DW_MMC_HC_PRIVATE_DATA *Private;
  EFI_STATUS             Status;

  if (This == NULL || Slot >= EXYNOS_DWMMC_MAX_SLOTS) {
    return EFI_INVALID_PARAMETER;
  }

  Private = DW_MMC_HC_PRIVATE_FROM_THIS (This);
  if (Private->Signature != DW_MMC_HC_PRIVATE_SIGNATURE) {
    return EFI_INVALID_PARAMETER;
  }

  Private->SlotInitialized[Slot] = FALSE;
  Status = DwMmcInitController (Private->SlotBase[Slot]);
  Private->SlotInitialized[Slot] = !EFI_ERROR (Status);
  return Status;
}

EFI_STATUS
EFIAPI
InitializeDwMmcHcDxe (
  IN EFI_HANDLE       ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  DW_MMC_HC_PRIVATE_DATA *Private;
  EFI_STATUS             Status;

  Private = AllocateZeroPool (sizeof (DW_MMC_HC_PRIVATE_DATA));
  if (Private == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Private->Signature                 = DW_MMC_HC_PRIVATE_SIGNATURE;
  Private->SlotBase[0]               = EXYNOS_DWMMC0_BASE;
  Private->SlotBase[1]               = EXYNOS_DWMMC2_BASE;
  Private->DevicePath                = (EFI_DEVICE_PATH_PROTOCOL *)&mDwMmcDevicePath;
  Private->PassThru.IoAlign          = 4;
  Private->PassThru.PassThru         = DwMmcPassThruPassThru;
  Private->PassThru.GetNextSlot      = DwMmcPassThruGetNextSlot;
  Private->PassThru.BuildDevicePath  = DwMmcPassThruBuildDevicePath;
  Private->PassThru.GetSlotNumber    = DwMmcPassThruGetSlotNumber;
  Private->PassThru.ResetDevice      = DwMmcPassThruResetDevice;

  // Initialiser le contrôleur eMMC interne (DWMMC0).
  Status = DwMmcInitController (Private->SlotBase[0]);
  Private->SlotInitialized[0] = !EFI_ERROR (Status);

  // Initialiser DWMMC2 seulement si une carte SD est détectée.
  if ((MmioRead32 (Private->SlotBase[1] + DW_MMC_CDETECT) & BIT0) == 0) {
    Status = DwMmcInitController (Private->SlotBase[1]);
    Private->SlotInitialized[1] = !EFI_ERROR (Status);
  }

  if (!Private->SlotInitialized[0] && !Private->SlotInitialized[1]) {
    FreePool (Private);
    return EFI_DEVICE_ERROR;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Private->Handle,
                  &gEfiSdMmcPassThruProtocolGuid, &Private->PassThru,
                  &gEfiDevicePathProtocolGuid,    Private->DevicePath,
                  NULL
                  );

  if (EFI_ERROR (Status)) {
    FreePool (Private);
  } else {
    gBS->ConnectController (Private->Handle, NULL, NULL, TRUE);
  }

  return Status;
}
