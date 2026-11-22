#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC
EFI_STATUS
WaitForKey (
  OUT EFI_INPUT_KEY *Key
  )
{
  UINTN Index;
  EFI_STATUS Status;

  Status = gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return gST->ConIn->ReadKeyStroke (gST->ConIn, Key);
}

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_INPUT_KEY Key;
  EFI_STATUS Status;

  while (TRUE) {
    gST->ConOut->ClearScreen (gST->ConOut);

    Print (L"========================================\n");
    Print (L"          7K - UEFI MULTIBOOT\n");
    Print (L"========================================\n\n");
    Print (L"  1. Android\n");
    Print (L"  2. Linux\n");
    Print (L"  3. Windows ARM64\n\n");
    Print (L"  ESC. Retour au gestionnaire UEFI\n\n");
    Print (L"Selection : ");

    Status = WaitForKey (&Key);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Key.ScanCode == SCAN_ESC) {
      return EFI_SUCCESS;
    }

    if (Key.UnicodeChar >= L'1' && Key.UnicodeChar <= L'3') {
      Print (L"\n\nChargeur selectionne : %c\n", Key.UnicodeChar);
      Print (L"Chargeur en cours de developpement.\n");
      Print (L"Appuyez sur une touche pour revenir.\n");
      Status = WaitForKey (&Key);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }
}
