#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>
#include <Guid/FileSystemInfo.h>

#define MAX_VOLUMES       32
#define MAX_ENTRIES       128
#define MAX_DEPTH         16
#define MAX_NAME_LENGTH   256
#define DIRECTORY_BUFFER  4096
#define MAX_EFI_IMAGE     0x10000000

typedef struct {
  EFI_HANDLE Handle;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *SimpleFs;
  EFI_FILE_PROTOCOL *Root;
  CHAR16 Label[64];
  UINT64 Size;
  UINT64 FreeSpace;
} MULTIBOOT_VOLUME;

typedef struct {
  CHAR16 Name[MAX_NAME_LENGTH];
  UINT64 Size;
  BOOLEAN IsDirectory;
  BOOLEAN IsEfi;
} FILE_ENTRY;

STATIC
EFI_STATUS
WaitForKey (
  OUT EFI_INPUT_KEY *Key
  )
{
  UINTN Index;
  EFI_STATUS Status;

  if (Key == NULL || gST->ConIn == NULL) {
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
CopyName (
  OUT CHAR16 *Destination,
  IN CONST CHAR16 *Source,
  IN UINTN MaxLength
  )
{
  UINTN Index;

  if (Destination == NULL || Source == NULL || MaxLength == 0) {
    return;
  }

  for (Index = 0; Index < MaxLength - 1 && Source[Index] != L'\0'; Index++) {
    Destination[Index] = Source[Index];
  }

  Destination[Index] = L'\0';
}

STATIC
BOOLEAN
IsEfiFile (
  IN CONST CHAR16 *Name
  )
{
  UINTN Length;
  CHAR16 A;
  CHAR16 B;
  CHAR16 C;

  if (Name == NULL) {
    return FALSE;
  }

  Length = 0;
  while (Name[Length] != L'\0') {
    Length++;
  }

  if (Length < 4) {
    return FALSE;
  }

  A = Name[Length - 4];
  B = Name[Length - 3];
  C = Name[Length - 2];

  if (A >= L'A' && A <= L'Z') A += L'a' - L'A';
  if (B >= L'A' && B <= L'Z') B += L'a' - L'A';
  if (C >= L'A' && C <= L'Z') C += L'a' - L'A';

  return (A == L'.' && B == L'e' && C == L'f' &&
          ((Name[Length - 1] == L'i') ||
           (Name[Length - 1] == L'I')));
}

STATIC
EFI_STATUS
ReadDirectory (
  IN EFI_FILE_PROTOCOL *Directory,
  OUT FILE_ENTRY *Entries,
  OUT UINTN *EntryCount
  )
{
  EFI_STATUS Status;
  EFI_FILE_INFO *Info;
  UINT8 *Buffer;
  UINTN BufferSize;
  UINTN Offset;
  UINTN EntrySize;

  if (Directory == NULL || Entries == NULL || EntryCount == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *EntryCount = 0;

  Buffer = NULL;
  Status = gBS->AllocatePool (
                  EfiBootServicesData,
                  DIRECTORY_BUFFER,
                  (VOID **)&Buffer
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Directory->SetPosition (Directory, 0);
  if (EFI_ERROR (Status)) {
    gBS->FreePool (Buffer);
    return Status;
  }

  while (*EntryCount < MAX_ENTRIES) {
    BufferSize = DIRECTORY_BUFFER;

    Status = Directory->Read (
                          Directory,
                          &BufferSize,
                          Buffer
                          );

    if (EFI_ERROR (Status)) {
      break;
    }

    if (BufferSize == 0) {
      Status = EFI_SUCCESS;
      break;
    }

    Offset = 0;

    while (Offset < BufferSize && *EntryCount < MAX_ENTRIES) {
      if (BufferSize - Offset < sizeof (EFI_FILE_INFO)) {
        Status = EFI_COMPROMISED_DATA;
        goto Done;
      }

      Info = (EFI_FILE_INFO *)(Buffer + Offset);
      EntrySize = (UINTN)Info->Size;

      if (EntrySize < sizeof (EFI_FILE_INFO) ||
          EntrySize > BufferSize - Offset) {
        Status = EFI_COMPROMISED_DATA;
        goto Done;
      }

      if (!(Info->FileName[0] == L'.' &&
            (Info->FileName[1] == L'\0' ||
             (Info->FileName[1] == L'.' &&
              Info->FileName[2] == L'\0')))) {

        CopyName (
          Entries[*EntryCount].Name,
          Info->FileName,
          MAX_NAME_LENGTH
          );

        Entries[*EntryCount].Size = Info->FileSize;
        Entries[*EntryCount].IsDirectory =
          (Info->Attribute & EFI_FILE_DIRECTORY) != 0;
        Entries[*EntryCount].IsEfi =
          IsEfiFile (Info->FileName);

        (*EntryCount)++;
      }

      Offset += EntrySize;
    }
  }

Done:
  gBS->FreePool (Buffer);
  return Status;
}

STATIC
EFI_STATUS
LaunchEfiFile (
  IN EFI_HANDLE ParentImage,
  IN EFI_FILE_PROTOCOL *Directory,
  IN CONST CHAR16 *FileName
  )
{
  EFI_STATUS Status;
  EFI_FILE_PROTOCOL *File;
  EFI_FILE_INFO *Info;
  UINTN InfoSize;
  UINTN ReadSize;
  UINTN TotalRead;
  UINT8 *ImageBuffer;
  EFI_HANDLE ChildImage;

  File = NULL;
  Info = NULL;
  ImageBuffer = NULL;
  ChildImage = NULL;

  Status = Directory->Open (
                        Directory,
                        &File,
                        (CHAR16 *)FileName,
                        EFI_FILE_MODE_READ,
                        0
                        );
  if (EFI_ERROR (Status)) {
    Print (L"Impossible d'ouvrir le fichier : %r\n", Status);
    return Status;
  }

  InfoSize = 0;
  Status = File->GetInfo (
                   File,
                   &gEfiFileInfoGuid,
                   &InfoSize,
                   NULL
                   );

  if (Status != EFI_BUFFER_TOO_SMALL || InfoSize == 0) {
    File->Close (File);
    return EFI_UNSUPPORTED;
  }

  Status = gBS->AllocatePool (
                  EfiBootServicesData,
                  InfoSize,
                  (VOID **)&Info
                  );
  if (EFI_ERROR (Status)) {
    File->Close (File);
    return Status;
  }

  Status = File->GetInfo (
                   File,
                   &gEfiFileInfoGuid,
                   &InfoSize,
                   Info
                   );

  if (EFI_ERROR (Status) ||
      Info->FileSize == 0 ||
      Info->FileSize > MAX_EFI_IMAGE) {
    Print (L"Taille de fichier invalide ou superieure a 256 Mo.\n");
    gBS->FreePool (Info);
    File->Close (File);
    return EFI_BAD_BUFFER_SIZE;
  }

  Status = gBS->AllocatePool (
                  EfiBootServicesData,
                  (UINTN)Info->FileSize,
                  (VOID **)&ImageBuffer
                  );

  if (EFI_ERROR (Status)) {
    gBS->FreePool (Info);
    File->Close (File);
    return Status;
  }

  TotalRead = 0;

  while (TotalRead < (UINTN)Info->FileSize) {
    ReadSize = (UINTN)Info->FileSize - TotalRead;

    Status = File->Read (
                     File,
                     &ReadSize,
                     ImageBuffer + TotalRead
                     );

    if (EFI_ERROR (Status) || ReadSize == 0) {
      Print (L"Erreur pendant la lecture du fichier.\n");
      Status = EFI_DEVICE_ERROR;
      goto Cleanup;
    }

    TotalRead += ReadSize;
  }

  Print (L"\nChargement de %s...\n", FileName);

  Status = gBS->LoadImage (
                  FALSE,
                  ParentImage,
                  NULL,
                  ImageBuffer,
                  TotalRead,
                  &ChildImage
                  );

  if (EFI_ERROR (Status)) {
    Print (L"LoadImage a echoue : %r\n", Status);
    goto Cleanup;
  }

  Print (L"Demarrage de l'application EFI...\n\n");

  Status = gBS->StartImage (
                  ChildImage,
                  NULL,
                  NULL
                  );

  Print (L"\nL'application EFI est revenue : %r\n", Status);

  gBS->UnloadImage (ChildImage);

Cleanup:
  if (ImageBuffer != NULL) {
    gBS->FreePool (ImageBuffer);
  }

  if (Info != NULL) {
    gBS->FreePool (Info);
  }

  if (File != NULL) {
    File->Close (File);
  }

  return Status;
}

STATIC
VOID
BrowseVolume (
  IN EFI_HANDLE ParentImage,
  IN MULTIBOOT_VOLUME *Volume
  )
{
  EFI_STATUS Status;
  EFI_FILE_PROTOCOL *DirectoryStack[MAX_DEPTH];
  UINTN Depth;
  FILE_ENTRY *Entries;
  UINTN EntryCount;
  UINTN Selected;
  UINTN Index;
  EFI_INPUT_KEY Key;
  EFI_FILE_PROTOCOL *Child;
  CHAR16 CurrentName[MAX_NAME_LENGTH];

  Entries = NULL;

  Status = gBS->AllocatePool (
                  EfiBootServicesData,
                  sizeof (FILE_ENTRY) * MAX_ENTRIES,
                  (VOID **)&Entries
                  );
  if (EFI_ERROR (Status)) {
    Print (L"Memoire insuffisante.\n");
    return;
  }

  for (Index = 0; Index < MAX_DEPTH; Index++) {
    DirectoryStack[Index] = NULL;
  }

  DirectoryStack[0] = Volume->Root;
  Depth = 0;
  Selected = 0;

  while (TRUE) {
    gST->ConOut->ClearScreen (gST->ConOut);

    Print (L"========================================\n");
    Print (L"          7K - EXPLORATEUR EFI\n");
    Print (L"========================================\n\n");

    Print (L"Volume : %s\n", Volume->Label);
    Print (L"Profondeur : %u\n\n", (UINT32)Depth);

    Status = ReadDirectory (
               DirectoryStack[Depth],
               Entries,
               &EntryCount
               );

    if (EFI_ERROR (Status)) {
      Print (L"Erreur de lecture du dossier : %r\n", Status);
      break;
    }

    if (EntryCount == 0) {
      Print (L"Dossier vide.\n");
    }

    if (Selected >= EntryCount && EntryCount > 0) {
      Selected = EntryCount - 1;
    }

    for (Index = 0; Index < EntryCount; Index++) {
      Print (
        L"%c %s %s",
        Index == Selected ? L'>' : L' ',
        Entries[Index].IsDirectory ? L"[DOSSIER]" :
          (Entries[Index].IsEfi ? L"[EFI]" : L"[FICHIER]"),
        Entries[Index].Name
        );

      if (!Entries[Index].IsDirectory) {
        Print (L"  (%Lu octets)", Entries[Index].Size);
      }

      Print (L"\n");
    }

    Print (L"\nHaut/Bas : Navigation | Entree : Ouvrir/Lancer\n");
    Print (L"Retour arriere : Dossier parent | ESC : Quitter\n");

    Status = WaitForKey (&Key);
    if (EFI_ERROR (Status)) {
      break;
    }

    if (Key.ScanCode == SCAN_ESC) {
      break;
    }

    if (Key.UnicodeChar == CHAR_BACKSPACE ||
        Key.UnicodeChar == 0x7F) {
      if (Depth > 0) {
        DirectoryStack[Depth]->Close (DirectoryStack[Depth]);
        DirectoryStack[Depth] = NULL;
        Depth--;
        Selected = 0;
      }
      continue;
    }

    if (Key.ScanCode == SCAN_UP) {
      if (EntryCount > 0) {
        Selected = (Selected == 0) ? EntryCount - 1 : Selected - 1;
      }
      continue;
    }

    if (Key.ScanCode == SCAN_DOWN) {
      if (EntryCount > 0) {
        Selected = (Selected + 1) % EntryCount;
      }
      continue;
    }

    if (Key.UnicodeChar != L'\r' || EntryCount == 0) {
      continue;
    }

    CopyName (
      CurrentName,
      Entries[Selected].Name,
      MAX_NAME_LENGTH
      );

    if (Entries[Selected].IsDirectory) {
      if (Depth + 1 >= MAX_DEPTH) {
        Print (L"Profondeur maximale atteinte.\n");
        continue;
      }

      Child = NULL;

      Status = DirectoryStack[Depth]->Open (
                                        DirectoryStack[Depth],
                                        &Child,
                                        CurrentName,
                                        EFI_FILE_MODE_READ,
                                        0
                                        );

      if (EFI_ERROR (Status) || Child == NULL) {
        Print (L"Impossible d'ouvrir ce dossier : %r\n", Status);
        continue;
      }

      Depth++;
      DirectoryStack[Depth] = Child;
      Selected = 0;
      continue;
    }

    if (!Entries[Selected].IsEfi) {
      Print (L"\nCe fichier n'est pas un executable .efi.\n");
      continue;
    }

    LaunchEfiFile (
      ParentImage,
      DirectoryStack[Depth],
      CurrentName
      );

    Print (L"\nAppuyez sur une touche pour continuer.\n");
    WaitForKey (&Key);
  }

  while (Depth > 0) {
    DirectoryStack[Depth]->Close (DirectoryStack[Depth]);
    Depth--;
  }

  gBS->FreePool (Entries);
}

STATIC
UINTN
FindInternalVolumes (
  OUT MULTIBOOT_VOLUME *Volumes
  )
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles;
  UINTN Count;
  UINTN Index;
  UINTN VolumeCount;
  EFI_BLOCK_IO_PROTOCOL *BlockIo;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *SimpleFs;
  EFI_FILE_PROTOCOL *Root;
  EFI_FILE_SYSTEM_INFO *FsInfo;
  UINTN InfoSize;

  Handles = NULL;
  Count = 0;
  VolumeCount = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );

  if (EFI_ERROR (Status) || Count == 0) {
    return 0;
  }

  for (Index = 0; Index < Count && VolumeCount < MAX_VOLUMES; Index++) {
    BlockIo = NULL;
    SimpleFs = NULL;
    Root = NULL;
    FsInfo = NULL;

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

    Status = SimpleFs->OpenVolume (SimpleFs, &Root);
    if (EFI_ERROR (Status) || Root == NULL) {
      continue;
    }

    InfoSize = 0;
    Status = Root->GetInfo (
                      Root,
                      &gEfiFileSystemInfoGuid,
                      &InfoSize,
                      NULL
                      );

    if (Status != EFI_BUFFER_TOO_SMALL || InfoSize == 0) {
      Root->Close (Root);
      continue;
    }

    Status = gBS->AllocatePool (
                    EfiBootServicesData,
                    InfoSize,
                    (VOID **)&FsInfo
                    );

    if (EFI_ERROR (Status) || FsInfo == NULL) {
      Root->Close (Root);
      continue;
    }

    Status = Root->GetInfo (
                      Root,
                      &gEfiFileSystemInfoGuid,
                      &InfoSize,
                      FsInfo
                      );

    if (EFI_ERROR (Status)) {
      gBS->FreePool (FsInfo);
      Root->Close (Root);
      continue;
    }

    Volumes[VolumeCount].Handle = Handles[Index];
    Volumes[VolumeCount].BlockIo = BlockIo;
    Volumes[VolumeCount].SimpleFs = SimpleFs;
    Volumes[VolumeCount].Root = Root;
    Volumes[VolumeCount].Size = FsInfo->VolumeSize;
    Volumes[VolumeCount].FreeSpace = FsInfo->FreeSpace;

    CopyName (
      Volumes[VolumeCount].Label,
      FsInfo->VolumeLabel,
      64
      );

    VolumeCount++;

    gBS->FreePool (FsInfo);
  }

  gBS->FreePool (Handles);

  return VolumeCount;
}

STATIC
VOID
ShowInternalVolumes (
  IN EFI_HANDLE ParentImage
  )
{
  EFI_STATUS Status;
  MULTIBOOT_VOLUME Volumes[MAX_VOLUMES];
  UINTN VolumeCount;
  UINTN Selected;
  UINTN Index;
  EFI_INPUT_KEY Key;

  VolumeCount = FindInternalVolumes (Volumes);

  if (VolumeCount == 0) {
    Print (L"Aucun volume interne lisible detecte.\n");
    WaitForKey (&Key);
    return;
  }

  Selected = 0;

  while (TRUE) {
    gST->ConOut->ClearScreen (gST->ConOut);

    Print (L"========================================\n");
    Print (L"          7K - VOLUMES INTERNES\n");
    Print (L"========================================\n\n");

    for (Index = 0; Index < VolumeCount; Index++) {
      Print (
        L"%c %u. %s | %Lu Mo | Libre : %Lu Mo\n",
        Index == Selected ? L'>' : L' ',
        (UINT32)(Index + 1),
        Volumes[Index].Label,
        Volumes[Index].Size / 1048576,
        Volumes[Index].FreeSpace / 1048576
        );
    }

    Print (L"\nHaut/Bas : Navigation | Entree : Ouvrir\n");
    Print (L"ESC : Retour au menu Multiboot\n");

    Status = WaitForKey (&Key);
    if (EFI_ERROR (Status) || Key.ScanCode == SCAN_ESC) {
      break;
    }

    if (Key.ScanCode == SCAN_UP) {
      Selected = (Selected == 0) ? VolumeCount - 1 : Selected - 1;
      continue;
    }

    if (Key.ScanCode == SCAN_DOWN) {
      Selected = (Selected + 1) % VolumeCount;
      continue;
    }

    if (Key.UnicodeChar == L'\r') {
      BrowseVolume (ParentImage, &Volumes[Selected]);
    }
  }

  for (Index = 0; Index < VolumeCount; Index++) {
    if (Volumes[Index].Root != NULL) {
      Volumes[Index].Root->Close (Volumes[Index].Root);
    }
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
  UINTN Selected;

  Selected = 0;

  while (TRUE) {
    gST->ConOut->ClearScreen (gST->ConOut);

    Print (L"========================================\n");
    Print (L"          7K - UEFI MULTIBOOT\n");
    Print (L"========================================\n\n");

    Print (L"  %c 1. Android\n", Selected == 0 ? L'>' : L' ');
    Print (L"  %c 2. Linux\n", Selected == 1 ? L'>' : L' ');
    Print (L"  %c 3. Windows ARM64\n\n", Selected == 2 ? L'>' : L' ');

    Print (L"  Haut/Bas : Navigation\n");
    Print (L"  Entree   : Choisir le systeme\n");
    Print (L"  ESC      : Retour au gestionnaire UEFI\n\n");

    Status = WaitForKey (&Key);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Key.ScanCode == SCAN_ESC) {
      return EFI_SUCCESS;
    }

    if (Key.ScanCode == SCAN_UP) {
      Selected = (Selected + 2) % 3;
      continue;
    }

    if (Key.ScanCode == SCAN_DOWN) {
      Selected = (Selected + 1) % 3;
      continue;
    }

    if (Key.UnicodeChar >= L'1' && Key.UnicodeChar <= L'3') {
      Selected = Key.UnicodeChar - L'1';
    } else if (Key.UnicodeChar != L'\r' &&
               Key.UnicodeChar != L'\n') {
      continue;
    }

    if (Selected == 0) {
      Print (L"\nAndroid necessitera un chargeur Android specifique.\n");
      Print (L"Les images Android ne sont pas des applications EFI.\n");
      Print (L"\nAppuyez sur une touche pour continuer.\n");
      WaitForKey (&Key);
      continue;
    }

    ShowInternalVolumes (ImageHandle);
  }
}
