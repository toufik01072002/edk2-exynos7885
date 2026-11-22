#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>

STATIC
EFI_STATUS
WaitForKey (
  OUT EFI_INPUT_KEY *Key
  )
{
  UINTN Index;
  EFI_STATUS Status;

  if (gST->ConIn == NULL || Key == NULL) {
    return EFI_NOT_READY;
  }

  while (TRUE) {
    Status = gBS->WaitForEvent (
                    1,
                    &gST->ConIn->WaitForKey,
                    &Index
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, Key);

    if (Status == EFI_NOT_READY) {
      continue;
    }

    return Status;
  }
}


STATIC
VOID
ShowInternalVolumes (
  VOID
  )
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles;
  UINTN Count;
  UINTN Index;
  UINTN InternalCount;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *SimpleFs;
  EFI_FILE_PROTOCOL *Root;

  Handles = NULL;
  Count = 0;
  InternalCount = 0;

  Print (L"\nDetection du stockage interne...\n\n");

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );

  if (EFI_ERROR (Status) || Count == 0) {
    Print (L"Aucun peripherique Block I/O detecte.\n");
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    BlockIo = NULL;
    SimpleFs = NULL;

    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiBlockIoProtocolGuid,
                    (VOID **)&BlockIo
                    );

    if (EFI_ERROR (Status) ||
        BlockIo == NULL ||
        BlockIo->Media == NULL ||
        !BlockIo->Media->MediaPresent ||
        BlockIo->Media->RemovableMedia) {
      continue;
    }

    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&SimpleFs
                    );

    if (EFI_ERROR (Status) || SimpleFs == NULL) {
      continue;
    }

    Root = NULL;

    Status = SimpleFs->OpenVolume (SimpleFs, &Root);

    if (EFI_ERROR (Status) || Root == NULL) {
      continue;
    }

    Root->Close (Root);

    InternalCount++;

    Print (
      L"Partition interne %u : bloc de %u octets\n",
      (UINT32)InternalCount,
      BlockIo->Media->BlockSize
      );
  }

  gBS->FreePool (Handles);

  if (InternalCount == 0) {
    Print (L"Aucune partition interne avec systeme de fichiers detectee.\n");
  } else {
    Print (L"\nTotal : %u partition(s) interne(s).\n",
           (UINT32)InternalCount);
  }
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
      Print (L"Detection des volumes disponibles.\n");
      ShowInternalVolumes ();
      Print (L"Appuyez sur une touche pour revenir.\n");
      Status = WaitForKey (&Key);
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }
}
