#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Pi/PiFirmwareVolume.h>
#include <Pi/PiFirmwareFile.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/LoadedImage.h>
#include <Guid/FileInfo.h>
#include <Guid/FileSystemInfo.h>
#include "AndroidImageLoader.h"

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
  UINTN AllocatedSize;
  UINTN EntrySize;
  UINTN NameChars;
  UINTN Index;

  if (Directory == NULL || Entries == NULL || EntryCount == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *EntryCount = 0;
  Buffer = NULL;
  AllocatedSize = DIRECTORY_BUFFER;

  Status = gBS->AllocatePool (
                  EfiBootServicesData,
                  AllocatedSize,
                  (VOID **)&Buffer
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Directory->SetPosition (Directory, 0);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  while (*EntryCount < MAX_ENTRIES) {
    BufferSize = AllocatedSize;

    Status = Directory->Read (
                          Directory,
                          &BufferSize,
                          Buffer
                          );

    if (Status == EFI_BUFFER_TOO_SMALL) {
      if (BufferSize <= AllocatedSize) {
        Status = EFI_COMPROMISED_DATA;
        break;
      }

      gBS->FreePool (Buffer);
      Buffer = NULL;
      AllocatedSize = BufferSize;

      Status = gBS->AllocatePool (
                      EfiBootServicesData,
                      AllocatedSize,
                      (VOID **)&Buffer
                      );
      if (EFI_ERROR (Status)) {
        break;
      }

      continue;
    }

    if (EFI_ERROR (Status)) {
      break;
    }

    if (BufferSize == 0) {
      Status = EFI_SUCCESS;
      break;
    }

    if (BufferSize < OFFSET_OF (EFI_FILE_INFO, FileName) + sizeof (CHAR16)) {
      Status = EFI_COMPROMISED_DATA;
      break;
    }

    Info = (EFI_FILE_INFO *)Buffer;

    if (Info->Size > MAX_UINTN ||
        Info->Size < OFFSET_OF (EFI_FILE_INFO, FileName) + sizeof (CHAR16) ||
        Info->Size > BufferSize) {
      Status = EFI_COMPROMISED_DATA;
      break;
    }

    EntrySize = (UINTN)Info->Size;
    NameChars =
      (EntrySize - OFFSET_OF (EFI_FILE_INFO, FileName)) / sizeof (CHAR16);

    for (Index = 0; Index < NameChars; Index++) {
      if (Info->FileName[Index] == L'\0') {
        break;
      }
    }

    if (Index == NameChars) {
      Status = EFI_COMPROMISED_DATA;
      break;
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

    Status = EFI_SUCCESS;
  }

Done:
  if (Buffer != NULL) {
    gBS->FreePool (Buffer);
  }

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


#define ANDROID_BOOT_HEADER_V0_SIZE  1632
#define ANDROID_BOOT_MAGIC           "ANDROID!"
#define ARM64_IMAGE_MAGIC            0x644D5241U

STATIC
UINT32
ReadLe32 (
  IN CONST UINT8 *Data
  )
{
  return (UINT32)Data[0] |
         ((UINT32)Data[1] << 8) |
         ((UINT32)Data[2] << 16) |
         ((UINT32)Data[3] << 24);
}

STATIC
UINT64
ReadLe64 (
  IN CONST UINT8 *Data
  )
{
  return (UINT64)ReadLe32 (Data) |
         ((UINT64)ReadLe32 (Data + 4) << 32);
}

STATIC
EFI_STATUS
ReadFileExact (
  IN EFI_FILE_PROTOCOL *File,
  IN UINT64 Offset,
  OUT VOID *Buffer,
  IN UINTN Size
  )
{
  EFI_STATUS Status;
  UINTN Total;
  UINTN Chunk;

  Status = File->SetPosition (File, Offset);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Total = 0;
  while (Total < Size) {
    Chunk = Size - Total;
    Status = File->Read (File, &Chunk, (UINT8 *)Buffer + Total);
    if (EFI_ERROR (Status) || Chunk == 0) {
      return EFI_DEVICE_ERROR;
    }

    Total += Chunk;
  }

  return EFI_SUCCESS;
}

STATIC
BOOLEAN
IsAndroidImageFile (
  IN CONST CHAR16 *Name
  )
{
  UINTN Length;
  CHAR16 A;
  CHAR16 B;
  CHAR16 C;
  CHAR16 D;

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
  D = Name[Length - 1];

  if (A >= L'A' && A <= L'Z') A += L'a' - L'A';
  if (B >= L'A' && B <= L'Z') B += L'a' - L'A';
  if (C >= L'A' && C <= L'Z') C += L'a' - L'A';
  if (D >= L'A' && D <= L'Z') D += L'a' - L'A';

  return (A == L'.' && B == L'i' && C == L'm' && D == L'g');
}

STATIC
EFI_STATUS
InspectAndroidBootImage (
  IN EFI_HANDLE ParentImage,
  IN EFI_FILE_PROTOCOL *Directory,
  IN CONST CHAR16 *FileName
  )
{
  EFI_STATUS Status;
  EFI_FILE_PROTOCOL *File;
  EFI_FILE_INFO *Info;
  UINTN InfoSize;
  UINT8 *Header;
  UINT8 KernelHeader[64];
  UINT32 KernelSize;
  UINT32 RamdiskSize;
  UINT32 SecondSize;
  UINT32 PageSize;
  UINT32 HeaderVersion;
  UINT64 KernelOffset;
  UINT64 RamdiskOffset;
  UINT64 SecondOffset;
  UINT64 ImageEnd;
  UINT64 AlignedSize;
  UINT64 KernelTextOffset;
  UINT64 KernelImageSize;
  UINT32 KernelFlags;

  File = NULL;
  Info = NULL;
  Header = NULL;

  Status = Directory->Open (
                        Directory,
                        &File,
                        (CHAR16 *)FileName,
                        EFI_FILE_MODE_READ,
                        0
                        );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  InfoSize = 0;
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL || InfoSize == 0) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  Status = gBS->AllocatePool (
                  EfiBootServicesData,
                  InfoSize,
                  (VOID **)&Info
                  );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (Info->FileSize < 2048) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }

  Status = gBS->AllocatePool (
                  EfiBootServicesData,
                  2048,
                  (VOID **)&Header
                  );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = ReadFileExact (File, 0, Header, 2048);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (Header[0] != 'A' || Header[1] != 'N' ||
      Header[2] != 'D' || Header[3] != 'R' ||
      Header[4] != 'O' || Header[5] != 'I' ||
      Header[6] != 'D' || Header[7] != '!') {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  KernelSize = ReadLe32 (Header + 8);
  RamdiskSize = ReadLe32 (Header + 16);
  SecondSize = ReadLe32 (Header + 24);
  PageSize = ReadLe32 (Header + 36);
  HeaderVersion = ReadLe32 (Header + 40);

  if (HeaderVersion != 0 ||
      PageSize < 2048 ||
      PageSize > 65536 ||
      (PageSize & (PageSize - 1)) != 0 ||
      KernelSize == 0 ||
      RamdiskSize == 0) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  if (KernelSize > ((UINT64)-1) - (PageSize - 1)) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }

  AlignedSize = ((UINT64)KernelSize + PageSize - 1) &
                ~((UINT64)PageSize - 1);
  KernelOffset = PageSize;

  if (KernelOffset > ((UINT64)-1) - AlignedSize) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }

  RamdiskOffset = KernelOffset + AlignedSize;

  if (RamdiskSize > ((UINT64)-1) - (PageSize - 1)) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }

  AlignedSize = ((UINT64)RamdiskSize + PageSize - 1) &
                ~((UINT64)PageSize - 1);

  if (RamdiskOffset > ((UINT64)-1) - AlignedSize) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }

  SecondOffset = RamdiskOffset + AlignedSize;

  if (SecondSize > ((UINT64)-1) - (PageSize - 1)) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }

  AlignedSize = ((UINT64)SecondSize + PageSize - 1) &
                ~((UINT64)PageSize - 1);

  if (SecondOffset > ((UINT64)-1) - AlignedSize) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }

  ImageEnd = SecondOffset + AlignedSize;
  if (ImageEnd > Info->FileSize) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }

  Status = ReadFileExact (File, KernelOffset, KernelHeader, sizeof (KernelHeader));
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (ReadLe32 (KernelHeader + 56) != ARM64_IMAGE_MAGIC) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  KernelTextOffset = ReadLe64 (KernelHeader + 8);
  KernelImageSize = ReadLe64 (KernelHeader + 16);
  KernelFlags = ReadLe32 (KernelHeader + 24);

  if (KernelImageSize < KernelSize ||
      KernelImageSize == 0 ||
      KernelImageSize > 0x40000000) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }

  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"========================================\n");
  Print (L"       ANDROID BOOT IMAGE - ANALYSE\n");
  Print (L"========================================\n\n");
  Print (L"Fichier       : %s\n", FileName);
  Print (L"Taille        : %Lu octets\n", Info->FileSize);
  Print (L"Format        : Android boot image v0\n");
  Print (L"Page size     : %u\n", PageSize);
  Print (L"Kernel offset : 0x%Lx\n", KernelOffset);
  Print (L"Kernel size   : %u octets\n", KernelSize);
  Print (L"Kernel addr*  : 0x%08x\n", ReadLe32 (Header + 12));
  Print (L"Image size    : 0x%Lx\n", KernelImageSize);
  Print (L"Text offset   : 0x%Lx\n", KernelTextOffset);
  Print (L"Kernel flags  : 0x%08x\n", KernelFlags);
  Print (L"Ramdisk off.  : 0x%Lx\n", RamdiskOffset);
  Print (L"Ramdisk size  : %u octets\n", RamdiskSize);
  Print (L"Ramdisk addr* : 0x%08x\n", ReadLe32 (Header + 20));
  Print (L"Second size   : %u octets\n", SecondSize);
  Print (L"\n* Adresses historiques du header : non utilisees comme adresses de chargement.\n");
  Print (L"\nImage validee. Chargement temporaire des composants...\n");
  Status = AndroidStageBootImage (ParentImage, Directory, FileName);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Print (L"\nChargement termine. Appuyez sur une touche pour revenir.\n");
  {
    EFI_INPUT_KEY Key;
    WaitForKey (&Key);
  }

  Status = EFI_SUCCESS;

Done:
  if (Header != NULL) {
    gBS->FreePool (Header);
  }

  if (Info != NULL) {
    gBS->FreePool (Info);
  }

  if (File != NULL) {
    File->Close (File);
  }

  if (EFI_ERROR (Status)) {
    Print (L"\nImage Android invalide ou format non supporte : %r\n", Status);
    {
      EFI_INPUT_KEY Key;
      WaitForKey (&Key);
    }
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
  BOOLEAN DirectoryLoaded;
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
  DirectoryLoaded = FALSE;

  while (TRUE) {
    gST->ConOut->ClearScreen (gST->ConOut);

    Print (L"========================================\n");
    Print (L"          7K - EXPLORATEUR EFI\n");
    Print (L"========================================\n\n");

    Print (L"Volume : %s\n", Volume->Label);
    Print (L"Profondeur : %u\n\n", (UINT32)Depth);

    if (!DirectoryLoaded) {
      Status = ReadDirectory (
                 DirectoryStack[Depth],
                 Entries,
                 &EntryCount
                 );

      if (EFI_ERROR (Status)) {
        Print (L"Erreur de lecture du dossier : %r\n", Status);
        Print (L"Appuyez sur une touche pour revenir.\n");
        WaitForKey (&Key);
        break;
      }

      DirectoryLoaded = TRUE;
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
        DirectoryLoaded = FALSE;
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
      DirectoryLoaded = FALSE;
      continue;
    }

    if (!Entries[Selected].IsEfi) {
      if (IsAndroidImageFile (CurrentName)) {
        InspectAndroidBootImage (
          ParentImage,
          DirectoryStack[Depth],
          CurrentName
          );
      } else {
        Print (L"\nCe fichier n'est pas un executable EFI ou une image Android .img.\n");
      }
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


#define MAX_LINUX_DISTROS  32

typedef struct {
  CHAR16 Name[64];
  CHAR16 VolumeLabel[64];
  EFI_FILE_PROTOCOL *Directory;
  BOOLEAN HasKernel;
  BOOLEAN HasInitrd;
  BOOLEAN HasDtb;
  BOOLEAN KernelValid;
  BOOLEAN InitrdValid;
  BOOLEAN DtbValid;
  UINT64 KernelSize;
  UINT64 InitrdSize;
  UINT64 DtbSize;
  CHAR16 InitrdFileName[64];
  CHAR16 DtbFileName[64];
} LINUX_DISTRO_ENTRY;

STATIC
BOOLEAN
InspectLinuxComponent (
  IN EFI_FILE_PROTOCOL *Directory,
  IN CONST CHAR16 *Name,
  IN UINTN ComponentType,
  OUT UINT64 *FileSize
  )
{
  EFI_STATUS Status;
  EFI_FILE_PROTOCOL *File;
  EFI_FILE_INFO *Info;
  UINTN InfoSize;
  UINT8 Header[64];
  BOOLEAN Valid;

  if (FileSize != NULL) {
    *FileSize = 0;
  }

  File = NULL;
  Info = NULL;
  Valid = FALSE;

  Status = Directory->Open (
                        Directory,
                        &File,
                        (CHAR16 *)Name,
                        EFI_FILE_MODE_READ,
                        0
                        );
  if (EFI_ERROR (Status) || File == NULL) {
    return FALSE;
  }

  InfoSize = 0;
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL || InfoSize == 0) {
    goto Done;
  }

  Status = gBS->AllocatePool (
                  EfiBootServicesData,
                  InfoSize,
                  (VOID **)&Info
                  );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status) ||
      (Info->Attribute & EFI_FILE_DIRECTORY) != 0 ||
      Info->FileSize == 0) {
    goto Done;
  }

  if (FileSize != NULL) {
    *FileSize = Info->FileSize;
  }

  if (ComponentType == 0) {
    Valid = TRUE;
  } else if (ComponentType == 1 && Info->FileSize >= sizeof (Header)) {
    Status = ReadFileExact (File, 0, Header, sizeof (Header));
    if (!EFI_ERROR (Status) &&
        ReadLe32 (Header + 56) == ARM64_IMAGE_MAGIC) {
      Valid = TRUE;
    }
  } else if (ComponentType == 2 && Info->FileSize >= 4) {
    Status = ReadFileExact (File, 0, Header, 4);
    if (!EFI_ERROR (Status) &&
        Header[0] == 0xD0 && Header[1] == 0x0D &&
        Header[2] == 0xFE && Header[3] == 0xED) {
      Valid = TRUE;
    }
  }

Done:
  if (Info != NULL) {
    gBS->FreePool (Info);
  }
  File->Close (File);
  return Valid;
}

STATIC
BOOLEAN
IsAlpineRootfsAccessible (
  IN EFI_FILE_PROTOCOL *VolumeRoot
  )
{
  EFI_STATUS Status;
  EFI_FILE_PROTOCOL *Rootfs;
  EFI_FILE_INFO *Info;
  UINTN InfoSize;
  BOOLEAN Accessible;

  if (VolumeRoot == NULL) {
    return FALSE;
  }

  Rootfs = NULL;
  Info = NULL;
  Accessible = FALSE;

  Status = VolumeRoot->Open (
                        VolumeRoot,
                        &Rootfs,
                        L"media\\0\\UEFI\\ALP\\rootfs",
                        EFI_FILE_MODE_READ,
                        0
                        );
  if (EFI_ERROR (Status) || Rootfs == NULL) {
    return FALSE;
  }

  InfoSize = 0;
  Status = Rootfs->GetInfo (
                     Rootfs,
                     &gEfiFileInfoGuid,
                     &InfoSize,
                     NULL
                     );
  if (Status == EFI_BUFFER_TOO_SMALL && InfoSize != 0) {
    Status = gBS->AllocatePool (
                    EfiBootServicesData,
                    InfoSize,
                    (VOID **)&Info
                    );
    if (!EFI_ERROR (Status)) {
      Status = Rootfs->GetInfo (
                         Rootfs,
                         &gEfiFileInfoGuid,
                         &InfoSize,
                         Info
                         );
      if (!EFI_ERROR (Status) &&
          (Info->Attribute & EFI_FILE_DIRECTORY) != 0) {
        Accessible = TRUE;
      }
    }
  }

  if (Info != NULL) {
    gBS->FreePool (Info);
  }
  Rootfs->Close (Rootfs);
  return Accessible;
}

STATIC CONST EFI_GUID mUefiShellFileGuid = {
  0x7C04A583, 0x9E3E, 0x4F1C,
  { 0xAD, 0x65, 0xE0, 0x52, 0x68, 0xD0, 0xB4, 0xD1 }
};

STATIC
EFI_STATUS
LaunchAlpineStartupScript (
  IN EFI_HANDLE ParentImage
  )
{
  EFI_STATUS Status;
  EFI_HANDLE *FvHandles;
  UINTN FvCount;
  UINTN Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL *Fv;
  VOID *ShellBuffer;
  UINTN ShellSize;
  UINT32 AuthenticationStatus;
  EFI_HANDLE ShellImage;
  EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
  CHAR16 *LoadOptions;
  CONST CHAR16 ShellArguments[] =
    L"-exit FS4:\\media\\0\\UEFI\\ALP\\startup.nsh";

  FvHandles = NULL;
  FvCount = 0;
  ShellBuffer = NULL;
  ShellSize = 0;
  ShellImage = NULL;
  LoadedImage = NULL;
  LoadOptions = NULL;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &FvCount,
                  &FvHandles
                  );
  if (EFI_ERROR (Status) || FvCount == 0) {
    Print (L"Firmware Volume introuvable : %r\n", Status);
    return EFI_NOT_FOUND;
  }

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < FvCount; Index++) {
    Fv = NULL;
    if (EFI_ERROR (gBS->HandleProtocol (
                         FvHandles[Index],
                         &gEfiFirmwareVolume2ProtocolGuid,
                         (VOID **)&Fv
                         )) || Fv == NULL) {
      continue;
    }

    ShellBuffer = NULL;
    ShellSize = 0;
    AuthenticationStatus = 0;
    Status = Fv->ReadSection (
                    Fv,
                    &mUefiShellFileGuid,
                    EFI_SECTION_PE32,
                    0,
                    &ShellBuffer,
                    &ShellSize,
                    &AuthenticationStatus
                    );
    if (!EFI_ERROR (Status) && ShellBuffer != NULL && ShellSize != 0) {
      break;
    }

    if (ShellBuffer != NULL) {
      gBS->FreePool (ShellBuffer);
      ShellBuffer = NULL;
    }
  }

  gBS->FreePool (FvHandles);
  if (EFI_ERROR (Status) || ShellBuffer == NULL || ShellSize == 0) {
    Print (L"Shell UEFI integre introuvable dans les volumes firmware : %r\n",
           Status);
    return EFI_NOT_FOUND;
  }

  Status = gBS->LoadImage (
                  FALSE,
                  ParentImage,
                  NULL,
                  ShellBuffer,
                  ShellSize,
                  &ShellImage
                  );
  gBS->FreePool (ShellBuffer);
  if (EFI_ERROR (Status)) {
    Print (L"Chargement du Shell UEFI impossible : %r\n", Status);
    return Status;
  }

  Status = gBS->HandleProtocol (
                  ShellImage,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  if (EFI_ERROR (Status) || LoadedImage == NULL) {
    Print (L"Protocole LoadedImage du Shell indisponible : %r\n", Status);
    gBS->UnloadImage (ShellImage);
    return Status;
  }

  Status = gBS->AllocatePool (
                  EfiBootServicesData,
                  sizeof (ShellArguments),
                  (VOID **)&LoadOptions
                  );
  if (EFI_ERROR (Status)) {
    gBS->UnloadImage (ShellImage);
    return Status;
  }

  CopyMem (LoadOptions, ShellArguments, sizeof (ShellArguments));
  LoadedImage->LoadOptions = LoadOptions;
  LoadedImage->LoadOptionsSize = sizeof (ShellArguments);

  Print (L"Lancement du Shell UEFI avec : %s\n", ShellArguments);
  Print (L"Script cible : FS4:\\media\\0\\UEFI\\ALP\\startup.nsh\n");

  Status = gBS->StartImage (ShellImage, NULL, NULL);

  Print (L"Retour du script startup.nsh : %r\n", Status);
  gBS->FreePool (LoadOptions);
  gBS->UnloadImage (ShellImage);
  return Status;
}

VOID
LaunchLinuxManager (
  IN EFI_HANDLE ImageHandle
  )
{
  EFI_STATUS Status;
  MULTIBOOT_VOLUME Volumes[MAX_VOLUMES];
  LINUX_DISTRO_ENTRY Distros[MAX_LINUX_DISTROS];
  FILE_ENTRY *Entries;
  UINTN VolumeCount;
  UINTN DistroCount;
  UINTN EntryCount;
  UINTN VolumeIndex;
  UINTN EntryIndex;
  UINTN Selected;
  UINTN Index;
  EFI_FILE_PROTOCOL *MultibootDirectory;
  EFI_FILE_PROTOCOL *LinuxDirectory;
  EFI_FILE_PROTOCOL *DistroDirectory;
  EFI_FILE_PROTOCOL *MediaDirectory;
  EFI_FILE_PROTOCOL *UserDirectory;
  EFI_FILE_PROTOCOL *UefiDirectory;
  EFI_FILE_PROTOCOL *AlpineDirectory;
  EFI_FILE_PROTOCOL *VirtualBootDirectory;
  EFI_INPUT_KEY Key;
  BOOLEAN Found;

  SetMem (Distros, sizeof (Distros), 0);
  Entries = NULL;
  DistroCount = 0;
  Selected = 0;
  MediaDirectory = NULL;
  UserDirectory = NULL;
  UefiDirectory = NULL;
  AlpineDirectory = NULL;
  VirtualBootDirectory = NULL;

  Status = gBS->AllocatePool (
                  EfiBootServicesData,
                  sizeof (FILE_ENTRY) * MAX_ENTRIES,
                  (VOID **)&Entries
                  );
  if (EFI_ERROR (Status)) {
    Print (L"Memoire insuffisante pour rechercher Linux.\n");
    return;
  }

  VolumeCount = FindInternalVolumes (Volumes);

  for (VolumeIndex = 0;
       VolumeIndex < VolumeCount && DistroCount < MAX_LINUX_DISTROS;
       VolumeIndex++) {
    MultibootDirectory = NULL;
    LinuxDirectory = NULL;

    Status = Volumes[VolumeIndex].Root->Open (
                                         Volumes[VolumeIndex].Root,
                                         &MultibootDirectory,
                                         L"Multiboot",
                                         EFI_FILE_MODE_READ,
                                         0
                                         );
    if (EFI_ERROR (Status) || MultibootDirectory == NULL) {
      continue;
    }

    Status = MultibootDirectory->Open (
                                  MultibootDirectory,
                                  &LinuxDirectory,
                                  L"Linux",
                                  EFI_FILE_MODE_READ,
                                  0
                                  );
    if (EFI_ERROR (Status) || LinuxDirectory == NULL) {
      MultibootDirectory->Close (MultibootDirectory);
      continue;
    }

    Status = ReadDirectory (LinuxDirectory, Entries, &EntryCount);
    if (!EFI_ERROR (Status)) {
      for (EntryIndex = 0;
           EntryIndex < EntryCount && DistroCount < MAX_LINUX_DISTROS;
           EntryIndex++) {
        if (!Entries[EntryIndex].IsDirectory) {
          continue;
        }

        DistroDirectory = NULL;
        Status = LinuxDirectory->Open (
                                  LinuxDirectory,
                                  &DistroDirectory,
                                  Entries[EntryIndex].Name,
                                  EFI_FILE_MODE_READ,
                                  0
                                  );
        if (EFI_ERROR (Status) || DistroDirectory == NULL) {
          continue;
        }

        Distros[DistroCount].KernelValid =
          InspectLinuxComponent (
            DistroDirectory, L"Image", 1,
            &Distros[DistroCount].KernelSize
            );
        Distros[DistroCount].HasKernel =
          Distros[DistroCount].KernelValid;
        if (!Distros[DistroCount].HasKernel) {
          Distros[DistroCount].HasKernel =
            InspectLinuxComponent (
              DistroDirectory, L"Image.gz", 0,
              &Distros[DistroCount].KernelSize
              );
          Distros[DistroCount].KernelValid = FALSE;
        }

        Distros[DistroCount].InitrdValid =
          InspectLinuxComponent (
            DistroDirectory, L"initramfs", 0,
            &Distros[DistroCount].InitrdSize
            );
        Distros[DistroCount].HasInitrd = Distros[DistroCount].InitrdValid;
        if (Distros[DistroCount].HasInitrd) {
          CopyName (Distros[DistroCount].InitrdFileName, L"initramfs", 64);
        } else {
          Distros[DistroCount].HasInitrd =
            InspectLinuxComponent (
              DistroDirectory, L"initrd.img", 0,
              &Distros[DistroCount].InitrdSize
              );
          Distros[DistroCount].InitrdValid = Distros[DistroCount].HasInitrd;
          if (Distros[DistroCount].HasInitrd) {
            CopyName (Distros[DistroCount].InitrdFileName, L"initrd.img", 64);
          }
        }

        Distros[DistroCount].DtbValid =
          InspectLinuxComponent (
            DistroDirectory, L"running.dtb", 2,
            &Distros[DistroCount].DtbSize
            );
        Distros[DistroCount].HasDtb = Distros[DistroCount].DtbValid;
        if (Distros[DistroCount].HasDtb) {
          CopyName (Distros[DistroCount].DtbFileName, L"running.dtb", 64);
        } else {
          Distros[DistroCount].DtbValid =
            InspectLinuxComponent (
              DistroDirectory, L"exynos7885-a205w.dtb", 2,
              &Distros[DistroCount].DtbSize
              );
          Distros[DistroCount].HasDtb = Distros[DistroCount].DtbValid;
          if (Distros[DistroCount].HasDtb) {
            CopyName (Distros[DistroCount].DtbFileName, L"exynos7885-a205w.dtb", 64);
          }
        }

        CopyName (
          Distros[DistroCount].Name,
          Entries[EntryIndex].Name,
          64
          );
        CopyName (
          Distros[DistroCount].VolumeLabel,
          Volumes[VolumeIndex].Label,
          64
          );
        Distros[DistroCount].Directory = DistroDirectory;
        DistroCount++;
      }
    }

    LinuxDirectory->Close (LinuxDirectory);
    MultibootDirectory->Close (MultibootDirectory);
  }

  //
  // Discover the Alpine installation stored on internal eMMC volume 5.
  // This is the same \media\0\UEFI tree already used by Android launch.
  //
  if (VolumeCount >= 5 && DistroCount < MAX_LINUX_DISTROS) {
    MediaDirectory = NULL;
    UserDirectory = NULL;
    UefiDirectory = NULL;
    AlpineDirectory = NULL;
    VirtualBootDirectory = NULL;

    Status = Volumes[4].Root->Open (
                                Volumes[4].Root,
                                &MediaDirectory,
                                L"media",
                                EFI_FILE_MODE_READ,
                                0
                                );
    if (!EFI_ERROR (Status) && MediaDirectory != NULL) {
      Status = MediaDirectory->Open (
                                 MediaDirectory,
                                 &UserDirectory,
                                 L"0",
                                 EFI_FILE_MODE_READ,
                                 0
                                 );
    }

    if (!EFI_ERROR (Status) && UserDirectory != NULL) {
      Status = UserDirectory->Open (
                                UserDirectory,
                                &UefiDirectory,
                                L"UEFI",
                                EFI_FILE_MODE_READ,
                                0
                                );
    }

    if (!EFI_ERROR (Status) && UefiDirectory != NULL) {
      Status = UefiDirectory->Open (
                                UefiDirectory,
                                &AlpineDirectory,
                                L"Alpine",
                                EFI_FILE_MODE_READ,
                                0
                                );
    }

    if (!EFI_ERROR (Status) && AlpineDirectory != NULL) {
      Status = AlpineDirectory->Open (
                                AlpineDirectory,
                                &VirtualBootDirectory,
                                L"virtual-boot",
                                EFI_FILE_MODE_READ,
                                0
                                );
    }

    if (!EFI_ERROR (Status) && VirtualBootDirectory != NULL) {
      Distros[DistroCount].KernelValid =
        InspectLinuxComponent (
          VirtualBootDirectory, L"Image", 1,
          &Distros[DistroCount].KernelSize
          );
      Distros[DistroCount].HasKernel =
        Distros[DistroCount].KernelValid;

      Distros[DistroCount].InitrdValid =
        InspectLinuxComponent (
          VirtualBootDirectory, L"initramfs-alpine.cpio.gz", 0,
          &Distros[DistroCount].InitrdSize
          );
      Distros[DistroCount].HasInitrd =
        Distros[DistroCount].InitrdValid;
      if (Distros[DistroCount].HasInitrd) {
        CopyName (
          Distros[DistroCount].InitrdFileName,
          L"initramfs-alpine.cpio.gz",
          64
          );
      }

      Distros[DistroCount].DtbValid =
        InspectLinuxComponent (
          VirtualBootDirectory, L"exynos7885-a205w-virtual.dtb", 2,
          &Distros[DistroCount].DtbSize
          );
      Distros[DistroCount].HasDtb =
        Distros[DistroCount].DtbValid;
      if (Distros[DistroCount].HasDtb) {
        CopyName (
          Distros[DistroCount].DtbFileName,
          L"exynos7885-a205w-virtual.dtb",
          64
          );
      }

      CopyName (Distros[DistroCount].Name, L"Alpine", 64);
      CopyName (
        Distros[DistroCount].VolumeLabel,
        Volumes[4].Label,
        64
        );
      Distros[DistroCount].Directory = VirtualBootDirectory;
      VirtualBootDirectory = NULL;
      DistroCount++;
    }

    if (VirtualBootDirectory != NULL) {
      VirtualBootDirectory->Close (VirtualBootDirectory);
      VirtualBootDirectory = NULL;
    }
    if (AlpineDirectory != NULL) {
      AlpineDirectory->Close (AlpineDirectory);
      AlpineDirectory = NULL;
    }
    if (UefiDirectory != NULL) {
      UefiDirectory->Close (UefiDirectory);
      UefiDirectory = NULL;
    }
    if (UserDirectory != NULL) {
      UserDirectory->Close (UserDirectory);
      UserDirectory = NULL;
    }
    if (MediaDirectory != NULL) {
      MediaDirectory->Close (MediaDirectory);
      MediaDirectory = NULL;
    }
  }

  if (DistroCount == 0) {
    gST->ConOut->ClearScreen (gST->ConOut);
    Print (L"========================================\n");
    Print (L"        GESTIONNAIRE LINUX ARM64\n");
    Print (L"========================================\n\n");
    Print (L"Aucune distribution detectee.\n\n");
    Print (L"Emplacement recherche sur les volumes internes :\n");
    Print (L"\\Multiboot\\Linux\\<Distribution>\\n\n");
    Print (L"Fichiers attendus : Image, initramfs, running.dtb\n");
    Print (L"Appuyez sur une touche pour revenir.\n");
    WaitForKey (&Key);
    goto Cleanup;
  }

  while (TRUE) {
    gST->ConOut->ClearScreen (gST->ConOut);
    Print (L"========================================\n");
    Print (L"        GESTIONNAIRE LINUX ARM64\n");
    Print (L"========================================\n\n");

    for (Index = 0; Index < DistroCount; Index++) {
      Print (
        L"%c %s  [Image:%s Initramfs:%s DTB:%s]\n",
        Index == Selected ? L'>' : L' ',
        Distros[Index].Name,
        Distros[Index].KernelValid ? L"ARM64" :
          (Distros[Index].HasKernel ? L"COMPRESSE" : L"--"),
        Distros[Index].InitrdValid ? L"OK" : L"--",
        Distros[Index].DtbValid ? L"FDT" : L"--"
        );
      if (Distros[Index].HasKernel) {
        Print (L"    Noyau : %Lu octets\n", Distros[Index].KernelSize);
      }
      if (Distros[Index].HasInitrd) {
        Print (L"    Initramfs : %Lu octets\n", Distros[Index].InitrdSize);
      }
      if (Distros[Index].HasDtb) {
        Print (L"    DTB : %Lu octets\n", Distros[Index].DtbSize);
      }
      Print (L"    Volume : %s\n", Distros[Index].VolumeLabel);
    }

    Print (L"\nHaut/Bas : Navigation | Entree : Selection | ESC : Retour\n");
    Status = WaitForKey (&Key);
    if (EFI_ERROR (Status) || Key.ScanCode == SCAN_ESC) {
      break;
    }

    if (Key.ScanCode == SCAN_UP) {
      Selected = (Selected == 0) ? DistroCount - 1 : Selected - 1;
      continue;
    }
    if (Key.ScanCode == SCAN_DOWN) {
      Selected = (Selected + 1) % DistroCount;
      continue;
    }
    if (Key.UnicodeChar != L'\r' && Key.UnicodeChar != L'\n') {
      continue;
    }

    gST->ConOut->ClearScreen (gST->ConOut);
    Print (L"Distribution : %s\n\n", Distros[Selected].Name);

    Found = Distros[Selected].KernelValid &&
            Distros[Selected].InitrdValid &&
            Distros[Selected].DtbValid;
    if (!Found) {
      Print (L"Configuration incomplete :\n");
      if (!Distros[Selected].HasKernel) Print (L" - Image ARM64 ou Image.gz manquant/invalide\n");
      else if (!Distros[Selected].KernelValid) Print (L" - Image.gz detecte : compression non prise en charge par cette validation\n");
      if (!Distros[Selected].HasInitrd) Print (L" - initramfs ou initrd.img absent/invalide\n");
      if (!Distros[Selected].HasDtb) Print (L" - DTB absent ou signature FDT invalide\n");
    } else {
      Print (L"Validation des formats :\n");
      Print (L" - Noyau ARM64 : signature validee\n");
      Print (L" - Initramfs : fichier non vide\n");
      Print (L" - DTB : signature FDT validee\n");
      Print (L" - Rootfs ALP \\media\\0\\UEFI\\ALP\\rootfs : ");
      if (VolumeCount >= 5 &&
          IsAlpineRootfsAccessible (Volumes[4].Root)) {
        Print (L"accessible (OK)\n");
      } else {
        Print (L"INTROUVABLE ou inaccessible\n");
        Status = EFI_NOT_FOUND;
        goto LinuxSelectionDone;
      }
      Print (L"\nDemarrage de la distribution Linux...\n");
      Status = LinuxStageComponents (
                 ImageHandle,
                 Distros[Selected].Directory,
                 Distros[Selected].InitrdFileName,
                 Distros[Selected].DtbFileName
                 );
      Print (L"Le transfert Linux a echoue : %r\n", Status);
    }

LinuxSelectionDone:
    Print (L"\nAppuyez sur une touche pour revenir a la liste.\n");
    WaitForKey (&Key);
  }

Cleanup:
  for (Index = 0; Index < DistroCount; Index++) {
    if (Distros[Index].Directory != NULL) {
      Distros[Index].Directory->Close (Distros[Index].Directory);
    }
  }

  for (Index = 0; Index < VolumeCount; Index++) {
    if (Volumes[Index].Root != NULL) {
      Volumes[Index].Root->Close (Volumes[Index].Root);
    }
  }

  if (Entries != NULL) {
    gBS->FreePool (Entries);
  }
}


STATIC
VOID
LaunchAndroidFromVolumes (
  IN EFI_HANDLE ParentImage
  )
{
  EFI_STATUS Status;
  MULTIBOOT_VOLUME Volumes[MAX_VOLUMES];
  UINTN VolumeCount;
  EFI_FILE_PROTOCOL *MediaDirectory;
  EFI_FILE_PROTOCOL *UserDirectory;
  EFI_FILE_PROTOCOL *AndroidDirectory;
  EFI_FILE_PROTOCOL *Probe;
  EFI_INPUT_KEY Key;

  MediaDirectory = NULL;
  UserDirectory = NULL;
  AndroidDirectory = NULL;
  Probe = NULL;

  VolumeCount = FindInternalVolumes (Volumes);
  if (VolumeCount < 5) {
    Print (L"Volume 5 indisponible : %u volumes internes detectes.\n",
           (UINT32)VolumeCount);
    goto Done;
  }

  Print (L"Recherche Android sur le volume 5 : \\media\\0\\UEFI\\...\n");

  Status = Volumes[4].Root->Open (
                              Volumes[4].Root,
                              &MediaDirectory,
                              L"media",
                              EFI_FILE_MODE_READ,
                              0
                              );
  if (EFI_ERROR (Status) || MediaDirectory == NULL) {
    Print (L"Le dossier \\media est introuvable sur le volume 5 : %r\n", Status);
    Print (L"Ouverture de l'explorateur du volume pour diagnostic.\n");
    BrowseVolume (ParentImage, &Volumes[4]);
    goto Done;
  }

  Status = MediaDirectory->Open (
                             MediaDirectory,
                             &UserDirectory,
                             L"0",
                             EFI_FILE_MODE_READ,
                             0
                             );
  if (EFI_ERROR (Status) || UserDirectory == NULL) {
    Print (L"Le dossier \\media\\0 est introuvable : %r\n", Status);
    goto Done;
  }

  Status = UserDirectory->Open (
                            UserDirectory,
                            &AndroidDirectory,
                            L"UEFI",
                            EFI_FILE_MODE_READ,
                            0
                            );
  if (EFI_ERROR (Status) || AndroidDirectory == NULL) {
    Print (L"Le dossier \\media\\0\\UEFI est introuvable : %r\n", Status);
    goto Done;
  }

  Status = AndroidDirectory->Open (
                              AndroidDirectory,
                              &Probe,
                              L"stock_boot.img",
                              EFI_FILE_MODE_READ,
                              0
                              );
  if (EFI_ERROR (Status) || Probe == NULL) {
    Print (L"stock_boot.img introuvable : %r\n", Status);
    goto Done;
  }
  Probe->Close (Probe);
  Probe = NULL;

  Status = AndroidDirectory->Open (
                              AndroidDirectory,
                              &Probe,
                              L"running.dtb",
                              EFI_FILE_MODE_READ,
                              0
                              );
  if (EFI_ERROR (Status) || Probe == NULL) {
    Print (L"running.dtb introuvable : %r\n", Status);
    goto Done;
  }
  Probe->Close (Probe);
  Probe = NULL;

  Print (L"Les deux fichiers Android sont presents sur le volume 5.\n");
  Status = InspectAndroidBootImage (
             ParentImage,
             AndroidDirectory,
             L"stock_boot.img"
             );
  if (EFI_ERROR (Status)) {
    Print (L"Le chargement Android a echoue : %r\n", Status);
  }

Done:
  if (Probe != NULL) {
    Probe->Close (Probe);
  }
  if (AndroidDirectory != NULL) {
    AndroidDirectory->Close (AndroidDirectory);
  }
  if (UserDirectory != NULL) {
    UserDirectory->Close (UserDirectory);
  }
  if (MediaDirectory != NULL) {
    MediaDirectory->Close (MediaDirectory);
  }

  {
    UINTN Index;
    for (Index = 0; Index < VolumeCount; Index++) {
      if (Volumes[Index].Root != NULL) {
        Volumes[Index].Root->Close (Volumes[Index].Root);
        Volumes[Index].Root = NULL;
      }
    }
  }

  Print (L"\nAppuyez sur une touche pour revenir au menu.\n");
  WaitForKey (&Key);
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
      LaunchAndroidFromVolumes (ImageHandle);
    } else if (Selected == 1) {
      Status = LaunchAlpineStartupScript (ImageHandle);
      if (EFI_ERROR (Status)) {
        Print (L"\nEchec du lancement startup.nsh : %r\n", Status);
        Print (L"Appuyez sur une touche pour revenir au menu.\n");
        WaitForKey (&Key);
      }
    } else {
      gST->ConOut->ClearScreen (gST->ConOut);
      Print (L"Le demarrage Windows ARM64 n'est pas encore integre.\n");
      Print (L"Appuyez sur une touche pour revenir au menu.\n");
      WaitForKey (&Key);
    }
  }
}
