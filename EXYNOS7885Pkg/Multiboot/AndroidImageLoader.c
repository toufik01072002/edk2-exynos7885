#include "AndroidImageLoader.h"
#include "GzipDecompress.h"

#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Guid/FileInfo.h>

#define ANDROID_PAGE_SIZE       2048U
#define ANDROID_HEADER_V0       0U
#define ANDROID_MAX_KERNEL      0x08000000ULL
#define ANDROID_MAX_KERNEL_MEM  0x10000000ULL
#define ANDROID_MAX_RAMDISK     0x04000000ULL
#define ARM64_IMAGE_MAGIC       0x644D5241U

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
ReadExact (
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
UINT32
ReadBe32 (
  IN CONST UINT8 *Data
  )
{
  return ((UINT32)Data[0] << 24) |
         ((UINT32)Data[1] << 16) |
         ((UINT32)Data[2] << 8) |
         (UINT32)Data[3];
}

STATIC
VOID
WriteBe32 (
  OUT UINT8 *Data,
  IN UINT32 Value
  )
{
  Data[0] = (UINT8)(Value >> 24);
  Data[1] = (UINT8)(Value >> 16);
  Data[2] = (UINT8)(Value >> 8);
  Data[3] = (UINT8)Value;
}

STATIC
VOID
WriteBe64 (
  OUT UINT8 *Data,
  IN UINT64 Value
  )
{
  WriteBe32 (Data, (UINT32)(Value >> 32));
  WriteBe32 (Data + 4, (UINT32)Value);
}

STATIC
BOOLEAN
FdtNameEquals (
  IN CONST CHAR8 *Name,
  IN UINTN NameLimit,
  IN CONST CHAR8 *Expected
  )
{
  UINTN Index;

  for (Index = 0; Expected[Index] != '\0'; Index++) {
    if (Index >= NameLimit || Name[Index] != Expected[Index]) {
      return FALSE;
    }
  }

  return (Index == NameLimit && Name[Index] == '\0');
}

STATIC
EFI_STATUS
PatchFdtInitrd (
  IN OUT UINT8 *Fdt,
  IN UINTN BufferSize,
  IN UINT64 InitrdStart,
  IN UINT64 InitrdEnd
  )
{
  UINT32 TotalSize;
  UINT32 StructOffset;
  UINT32 StringsOffset;
  UINT32 StringsSize;
  UINT32 StructSize;
  UINT32 Position;
  UINT32 StructEnd;
  UINT32 Token;
  UINT32 PropertySize;
  UINT32 NameOffset;
  UINT32 NameLength;
  UINT32 NodeLength;
  UINT32 Depth;
  UINT32 ChosenDepth;
  UINT32 Index;
  UINT8 *PropertyData;
  CONST CHAR8 *PropertyName;
  BOOLEAN FoundStart;
  BOOLEAN FoundEnd;
  UINT64 Value;

  if (Fdt == NULL || BufferSize < 40 ||
      ReadBe32 (Fdt) != 0xD00DFEEDU) {
    return EFI_UNSUPPORTED;
  }

  TotalSize = ReadBe32 (Fdt + 4);
  StructOffset = ReadBe32 (Fdt + 8);
  StringsOffset = ReadBe32 (Fdt + 12);
  if (ReadBe32 (Fdt + 20) < 17) {
    return EFI_UNSUPPORTED;
  }
  StringsSize = ReadBe32 (Fdt + 32);
  StructSize = ReadBe32 (Fdt + 36);

  if (TotalSize < 40 || TotalSize > BufferSize ||
      StructOffset > TotalSize || StructSize > TotalSize - StructOffset ||
      StringsOffset > TotalSize || StringsSize > TotalSize - StringsOffset ||
      (StructOffset & 3) != 0) {
    return EFI_COMPROMISED_DATA;
  }

  StructEnd = StructOffset + StructSize;
  Position = StructOffset;
  Depth = 0;
  ChosenDepth = 0;
  FoundStart = FALSE;
  FoundEnd = FALSE;

  while (Position <= StructEnd && StructEnd - Position >= 4) {
    Token = ReadBe32 (Fdt + Position);
    Position += 4;

    if (Token == 1) {
      Index = Position;
      while (Index < StructEnd && Fdt[Index] != 0) {
        Index++;
      }
      if (Index >= StructEnd) {
        return EFI_COMPROMISED_DATA;
      }

      NodeLength = Index - Position;
      if (Depth == 1 && NodeLength == 6 &&
          CompareMem (Fdt + Position, "chosen", 6) == 0) {
        ChosenDepth = Depth + 1;
      }

      Position = (Index + 1 + 3) & ~3U;
      if (Position > StructEnd || Depth == MAX_UINT32) {
        return EFI_COMPROMISED_DATA;
      }
      Depth++;
    } else if (Token == 2) {
      if (Depth == 0) {
        return EFI_COMPROMISED_DATA;
      }
      if (Depth == ChosenDepth) {
        ChosenDepth = 0;
      }
      Depth--;
    } else if (Token == 3) {
      if (StructEnd - Position < 8) {
        return EFI_COMPROMISED_DATA;
      }
      PropertySize = ReadBe32 (Fdt + Position);
      NameOffset = ReadBe32 (Fdt + Position + 4);
      Position += 8;

      if (PropertySize > StructEnd - Position ||
          NameOffset >= StringsSize) {
        return EFI_COMPROMISED_DATA;
      }

      PropertyName = (CONST CHAR8 *)(Fdt + StringsOffset + NameOffset);
      NameLength = 0;
      while (NameOffset + NameLength < StringsSize &&
             PropertyName[NameLength] != '\0') {
        NameLength++;
      }
      if (NameOffset + NameLength >= StringsSize) {
        return EFI_COMPROMISED_DATA;
      }

      if (Depth == ChosenDepth && ChosenDepth != 0) {
        PropertyData = Fdt + Position;
        if (FdtNameEquals (PropertyName, NameLength, "linux,initrd-start")) {
          if (PropertySize == 4) {
            if (InitrdStart > MAX_UINT32) {
              return EFI_BAD_BUFFER_SIZE;
            }
            WriteBe32 (PropertyData, (UINT32)InitrdStart);
          } else if (PropertySize == 8) {
            WriteBe64 (PropertyData, InitrdStart);
          } else {
            return EFI_UNSUPPORTED;
          }
          FoundStart = TRUE;
        } else if (FdtNameEquals (PropertyName, NameLength, "linux,initrd-end")) {
          if (PropertySize == 4) {
            if (InitrdEnd > MAX_UINT32) {
              return EFI_BAD_BUFFER_SIZE;
            }
            WriteBe32 (PropertyData, (UINT32)InitrdEnd);
          } else if (PropertySize == 8) {
            WriteBe64 (PropertyData, InitrdEnd);
          } else {
            return EFI_UNSUPPORTED;
          }
          FoundEnd = TRUE;
        }
      }

      Position = (Position + PropertySize + 3) & ~3U;
      if (Position > StructEnd) {
        return EFI_COMPROMISED_DATA;
      }
    } else if (Token == 4) {
      continue;
    } else if (Token == 9) {
      break;
    } else {
      return EFI_COMPROMISED_DATA;
    }
  }

  if (!FoundStart || !FoundEnd) {
    return EFI_NOT_FOUND;
  }

  Value = InitrdEnd - InitrdStart;
  if (Value == 0 || InitrdEnd < InitrdStart) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
LoadAndPatchFdt (
  IN EFI_FILE_PROTOCOL *Directory,
  IN UINT64 InitrdStart,
  IN UINT64 InitrdEnd,
  OUT UINT8 **FdtBuffer,
  OUT UINTN *FdtBufferSize
  )
{
  EFI_STATUS Status;
  EFI_FILE_PROTOCOL *File;
  EFI_FILE_INFO *Info;
  UINTN InfoSize;
  UINT8 Header[40];
  UINT32 TotalSize;
  UINT8 *Buffer;

  if (Directory == NULL || FdtBuffer == NULL || FdtBufferSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *FdtBuffer = NULL;
  *FdtBufferSize = 0;
  File = NULL;
  Info = NULL;
  Buffer = NULL;

  Status = Directory->Open (
                        Directory,
                        &File,
                        L"running.dtb",
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

  if (Info->FileSize < sizeof (Header) ||
      Info->FileSize > 0x00200000ULL ||
      Info->FileSize > MAX_UINTN) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }

  Status = ReadExact (File, 0, Header, sizeof (Header));
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (ReadBe32 (Header) != 0xD00DFEEDU) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  TotalSize = ReadBe32 (Header + 4);
  if (TotalSize < sizeof (Header) || TotalSize > Info->FileSize) {
    Status = EFI_COMPROMISED_DATA;
    goto Done;
  }

  Status = gBS->AllocatePool (
                  EfiBootServicesData,
                  TotalSize,
                  (VOID **)&Buffer
                  );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = ReadExact (File, 0, Buffer, TotalSize);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = PatchFdtInitrd (Buffer, TotalSize, InitrdStart, InitrdEnd);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  *FdtBuffer = Buffer;
  *FdtBufferSize = TotalSize;
  Buffer = NULL;
  Status = EFI_SUCCESS;

Done:
  if (Buffer != NULL) {
    gBS->FreePool (Buffer);
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
UINT64
AlignUp (
  IN UINT64 Value,
  IN UINT32 Alignment
  )
{
  return (Value + Alignment - 1) & ~((UINT64)Alignment - 1);
}

EFI_STATUS
EFIAPI
AndroidStageBootImage (
  IN EFI_FILE_PROTOCOL *Directory,
  IN CONST CHAR16 *FileName
  )
{
  EFI_STATUS Status;
  EFI_FILE_PROTOCOL *File;
  EFI_FILE_INFO *Info;
  UINTN InfoSize;
  UINT8 Header[ANDROID_PAGE_SIZE];
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
  UINT64 KernelTextOffset;
  UINT64 KernelImageSize;
  UINT64 KernelPages;
  UINT64 RamdiskPages;
  UINTN EventIndex;
  EFI_INPUT_KEY Key;
  UINT8 *FdtBuffer;
  UINTN FdtBufferSize;
  UINTN FdtPages;
  EFI_PHYSICAL_ADDRESS FdtAddress;
  EFI_PHYSICAL_ADDRESS MaxAddress;
  EFI_PHYSICAL_ADDRESS KernelAddress;
  EFI_PHYSICAL_ADDRESS RamdiskAddress;
  EFI_PHYSICAL_ADDRESS DecompressedRamdiskAddress;
  UINTN DecompressedRamdiskSize;
  UINTN DecompressedRamdiskPages;
  BOOLEAN KernelAllocated;
  BOOLEAN RamdiskAllocated;
  BOOLEAN DecompressedRamdiskAllocated;
  BOOLEAN FdtAllocated;

  File = NULL;
  Info = NULL;
  KernelAddress = 0;
  RamdiskAddress = 0;
  DecompressedRamdiskAddress = 0;
  DecompressedRamdiskSize = 0;
  DecompressedRamdiskPages = 0;
  FdtBuffer = NULL;
  FdtBufferSize = 0;
  FdtPages = 0;
  FdtAddress = 0;
  KernelAllocated = FALSE;
  RamdiskAllocated = FALSE;
  DecompressedRamdiskAllocated = FALSE;
  FdtAllocated = FALSE;
  KernelPages = 0;
  RamdiskPages = 0;

  if (Directory == NULL || FileName == NULL) {
    return EFI_INVALID_PARAMETER;
  }

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

  if (Info->FileSize < ANDROID_PAGE_SIZE ||
      Info->FileSize > 0x10000000ULL) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }

  Status = ReadExact (File, 0, Header, sizeof (Header));
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (CompareMem (Header, "ANDROID!", 8) != 0) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  KernelSize = ReadLe32 (Header + 8);
  RamdiskSize = ReadLe32 (Header + 16);
  SecondSize = ReadLe32 (Header + 24);
  PageSize = ReadLe32 (Header + 36);
  HeaderVersion = ReadLe32 (Header + 40);

  if (HeaderVersion != ANDROID_HEADER_V0 ||
      PageSize < ANDROID_PAGE_SIZE ||
      PageSize > 65536 ||
      (PageSize & (PageSize - 1)) != 0 ||
      KernelSize == 0 || KernelSize > ANDROID_MAX_KERNEL ||
      RamdiskSize == 0 || RamdiskSize > ANDROID_MAX_RAMDISK) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  KernelOffset = PageSize;
  RamdiskOffset = KernelOffset + AlignUp (KernelSize, PageSize);
  SecondOffset = RamdiskOffset + AlignUp (RamdiskSize, PageSize);
  ImageEnd = SecondOffset + AlignUp (SecondSize, PageSize);

  if (RamdiskOffset < KernelOffset ||
      SecondOffset < RamdiskOffset ||
      ImageEnd < SecondOffset ||
      ImageEnd > Info->FileSize) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }

  Status = ReadExact (File, KernelOffset, KernelHeader, sizeof (KernelHeader));
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (ReadLe32 (KernelHeader + 56) != ARM64_IMAGE_MAGIC) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  KernelTextOffset = ReadLe64 (KernelHeader + 8);
  KernelImageSize = ReadLe64 (KernelHeader + 16);
  if (KernelImageSize < KernelSize ||
      KernelImageSize > ANDROID_MAX_KERNEL_MEM ||
      KernelTextOffset > 0x200000) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }

  Status = ReadExact (File, RamdiskOffset, Header, 2);
  if (EFI_ERROR (Status)) {
    goto Done;
  }
  if (Header[0] != 0x1F || Header[1] != 0x8B) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }

  KernelPages = EFI_SIZE_TO_PAGES ((UINTN)KernelImageSize);
  RamdiskPages = EFI_SIZE_TO_PAGES ((UINTN)RamdiskSize);

  Status = gBS->AllocatePages (
                  AllocateAnyPages,
                  EfiLoaderData,
                  (UINTN)KernelPages,
                  &KernelAddress
                  );
  if (EFI_ERROR (Status)) {
    goto Done;
  }
  KernelAllocated = TRUE;

  MaxAddress = 0xFFFFFFFFULL;
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiLoaderData,
                  (UINTN)RamdiskPages,
                  &MaxAddress
                  );
  RamdiskAddress = MaxAddress;
  if (EFI_ERROR (Status)) {
    goto Done;
  }
  RamdiskAllocated = TRUE;

  SetMem ((VOID *)(UINTN)KernelAddress, (UINTN)EFI_PAGES_TO_SIZE (KernelPages), 0);
  Status = ReadExact (
             File,
             KernelOffset,
             (VOID *)(UINTN)KernelAddress,
             KernelSize
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = ReadExact (
             File,
             RamdiskOffset,
             (VOID *)(UINTN)RamdiskAddress,
             RamdiskSize
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = GzipDecompress (
             (CONST UINT8 *)(UINTN)RamdiskAddress,
             RamdiskSize,
             &DecompressedRamdiskAddress,
             &DecompressedRamdiskSize,
             &DecompressedRamdiskPages
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }
  DecompressedRamdiskAllocated = TRUE;

  Status = LoadAndPatchFdt (
             Directory,
             DecompressedRamdiskAddress,
             DecompressedRamdiskAddress + DecompressedRamdiskSize,
             &FdtBuffer,
             &FdtBufferSize
             );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  FdtPages = EFI_SIZE_TO_PAGES (FdtBufferSize);
  MaxAddress = 0xFFFFFFFFULL;
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiLoaderData,
                  FdtPages,
                  &MaxAddress
                  );
  if (EFI_ERROR (Status)) {
    goto Done;
  }
  FdtAddress = MaxAddress;
  FdtAllocated = TRUE;
  CopyMem (
    (VOID *)(UINTN)FdtAddress,
    FdtBuffer,
    FdtBufferSize
    );

  Print (L"\nChargement temporaire reussi.\n");
  Print (L"Noyau ARM64 : %u octets, memoire image : 0x%Lx\n",
         KernelSize, KernelImageSize);
  Print (L"Ramdisk gzip : %u octets\n", RamdiskSize);
  Print (L"Ramdisk CPIO decompresse : %u octets\n", (UINT32)DecompressedRamdiskSize);
  Print (L"DTB actif patche : %u octets\n", (UINT32)FdtBufferSize);
  Print (L"Adresse tampon noyau : 0x%Lx\n", KernelAddress);
  Print (L"Adresse tampon gzip : 0x%Lx\n", RamdiskAddress);
  Print (L"Adresse ramdisk CPIO : 0x%Lx\n", DecompressedRamdiskAddress);
  Print (L"Adresse DTB temporaire : 0x%Lx\n", FdtAddress);
  Print (L"DTB : linux,initrd-start/end actualises, bootargs conserves.\n");
  Print (L"Ces adresses sont temporaires, pas les adresses finales de boot.\n");
  Print (L"Aucun transfert au noyau n'a ete effectue.\n");
  Print (L"Appuyez sur une touche pour liberer les tampons.\\n");
  do {
    Status = gBS->WaitForEvent (
                    1,
                    &gST->ConIn->WaitForKey,
                    &EventIndex
                    );
    if (EFI_ERROR (Status)) {
      goto Done;
    }
    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
  } while (Status == EFI_NOT_READY);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = EFI_SUCCESS;

Done:
  if (FdtAllocated) {
    gBS->FreePages (FdtAddress, FdtPages);
  }
  if (FdtBuffer != NULL) {
    gBS->FreePool (FdtBuffer);
  }
  if (DecompressedRamdiskAllocated) {
    gBS->FreePages (DecompressedRamdiskAddress, DecompressedRamdiskPages);
  }
  if (RamdiskAllocated) {
    gBS->FreePages (RamdiskAddress, (UINTN)RamdiskPages);
  }
  if (KernelAllocated) {
    gBS->FreePages (KernelAddress, (UINTN)KernelPages);
  }
  if (Info != NULL) {
    gBS->FreePool (Info);
  }
  if (File != NULL) {
    File->Close (File);
  }

  if (EFI_ERROR (Status)) {
    Print (L"\nEchec du chargement Android : %r\n", Status);
  }
  return Status;
}
