#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>

#define KERNEL_PATH L"media\\0\\UEFI\\Alpine\\virtual-boot\\Image"
#define INITRD_PATH L"media\\0\\UEFI\\Alpine\\virtual-boot\\initramfs-alpine.cpio.gz"
#define DTB_PATH L"media\\0\\UEFI\\Alpine\\virtual-boot\\exynos7885-a205w-virtual.dtb"
#define ROOTFS_PATH L"media\\0\\UEFI\\ALP\\rootfs"
#define ARM64_IMAGE_MAGIC 0x644D5241U
#define FDT_MAGIC 0xD00DFEEDU

STATIC UINT16 ReadLe16(CONST UINT8 *P) {
  return (UINT16)P[0] | ((UINT16)P[1] << 8);
}
STATIC UINT32 ReadLe32(CONST UINT8 *P) {
  return (UINT32)P[0] | ((UINT32)P[1] << 8) |
         ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}
STATIC UINT32 ReadBe32(CONST UINT8 *P) {
  return ((UINT32)P[0] << 24) | ((UINT32)P[1] << 16) |
         ((UINT32)P[2] << 8) | (UINT32)P[3];
}
STATIC EFI_STATUS OpenInfo(EFI_FILE_PROTOCOL *Root, CONST CHAR16 *Path,
                           EFI_FILE_PROTOCOL **File, EFI_FILE_INFO **Info) {
  EFI_STATUS S; UINTN N=0;
  *File=NULL; *Info=NULL;
  S=Root->Open(Root,File,(CHAR16 *)Path,EFI_FILE_MODE_READ,0);
  if(EFI_ERROR(S)) return S;
  S=(*File)->GetInfo(*File,&gEfiFileInfoGuid,&N,NULL);
  if(S!=EFI_BUFFER_TOO_SMALL || N==0) return EFI_UNSUPPORTED;
  S=gBS->AllocatePool(EfiBootServicesData,N,(VOID **)Info);
  if(EFI_ERROR(S)) return S;
  S=(*File)->GetInfo(*File,&gEfiFileInfoGuid,&N,*Info);
  return S;
}
STATIC VOID CloseInfo(EFI_FILE_PROTOCOL *F, EFI_FILE_INFO *I) {
  if(I) gBS->FreePool(I);
  if(F) F->Close(F);
}
EFI_STATUS EFIAPI UefiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS S; EFI_LOADED_IMAGE_PROTOCOL *Loaded=NULL;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs=NULL; EFI_FILE_PROTOCOL *Root=NULL;
  EFI_FILE_PROTOCOL *F=NULL; EFI_FILE_INFO *I=NULL;
  UINT8 H[64]; UINT8 Pe[26]; UINTN N; UINT64 KernelSize, ImageSize, TextOffset; UINT32 PeOffset; UINT16 PeMachine, PeOptionalMagic;
  UINT8 Gzip[2]; UINT8 Fdt[40]; UINT32 FdtSize;
  BOOLEAN Failed=FALSE;

  Print(L"\n=== A205W LINUX CHECK / READ-ONLY ===\n");
  S=gBS->HandleProtocol(ImageHandle,&gEfiLoadedImageProtocolGuid,(VOID **)&Loaded);
  if(EFI_ERROR(S) || Loaded==NULL) { Print(L"[FAIL] LoadedImage: %r\n",S); return S; }
  S=gBS->HandleProtocol(Loaded->DeviceHandle,&gEfiSimpleFileSystemProtocolGuid,(VOID **)&Fs);
  if(EFI_ERROR(S) || Fs==NULL) { Print(L"[FAIL] SimpleFS: %r\n",S); return S; }
  S=Fs->OpenVolume(Fs,&Root);
  if(EFI_ERROR(S) || Root==NULL) { Print(L"[FAIL] OpenVolume: %r\n",S); return S; }
  Print(L"[OK] Volume de lancement ouvert (meme volume que l'application EFI).\n");

  S=OpenInfo(Root,KERNEL_PATH,&F,&I);
  if(EFI_ERROR(S)) { Print(L"[FAIL] Image: %r\n",S); Failed=TRUE; }
  else {
    KernelSize=I->FileSize; N=sizeof(H);
    S=F->SetPosition(F,0);
    if(!EFI_ERROR(S)) S=F->Read(F,&N,H);
    if(EFI_ERROR(S) || N!=sizeof(H) || KernelSize<sizeof(H) ||
       ReadLe32(H+56)!=ARM64_IMAGE_MAGIC) {
      Print(L"[FAIL] Image ARM64 header (size=%Lu, status=%r)\n",KernelSize,S); Failed=TRUE;
    } else {
      TextOffset=(UINT64)ReadLe32(H+8) | ((UINT64)ReadLe32(H+12)<<32);
      ImageSize=(UINT64)ReadLe32(H+16) | ((UINT64)ReadLe32(H+20)<<32);
      Print(L"[OK] Image: %Lu bytes, ARM64 magic valide\n",KernelSize);
      Print(L"     text_offset=0x%Lx image_size=0x%Lx flags=0x%Lx\n",
            TextOffset,ImageSize,(UINT64)ReadLe32(H+24)|((UINT64)ReadLe32(H+28)<<32));
      if(ImageSize==0 || ImageSize<KernelSize) { Print(L"[FAIL] image_size incoherent\n"); Failed=TRUE; }

      //
      // ARM64 Image.res5 at offset 60 contains the PE/COFF header offset
      // when CONFIG_EFI_STUB is enabled. This is a read-only probe.
      //
      PeOffset=ReadLe32(H+60);
      Print(L"     res5/PE offset=0x%08x\n",PeOffset);
      if(ReadLe32(H)==0x91005A4DU && PeOffset>=64 &&
         (UINT64)PeOffset+sizeof(Pe)<=KernelSize) {
        N=sizeof(Pe);
        S=F->SetPosition(F,PeOffset);
        if(!EFI_ERROR(S)) S=F->Read(F,&N,Pe);
        if(!EFI_ERROR(S) && N==sizeof(Pe) &&
           Pe[0]=='P' && Pe[1]=='E' && Pe[2]==0 && Pe[3]==0) {
          PeMachine=ReadLe16(Pe+4);
          PeOptionalMagic=ReadLe16(Pe+24);
          Print(L"[PE] signature valide, machine=0x%04x, optional=0x%04x\n",
                PeMachine,PeOptionalMagic);
          if(PeMachine==0xAA64 && PeOptionalMagic==0x020B) {
            Print(L"[OK] En-tete PE32+ AArch64 detecte : EFI Stub probablement present.\n");
          } else {
            Print(L"[WARN] PE present mais architecture/format inattendu.\n");
          }
        } else {
          Print(L"[NO] Aucun en-tete PE valide a l'offset annonce.\n");
        }
      } else {
        Print(L"[NO] Pas d'en-tete PE ARM64 annonce (EFI Stub non confirme).\n");
      }
    }
    CloseInfo(F,I); F=NULL; I=NULL;
  }

  S=OpenInfo(Root,INITRD_PATH,&F,&I);
  if(EFI_ERROR(S)) { Print(L"[FAIL] initramfs: %r\n",S); Failed=TRUE; }
  else {
    N=2; S=F->SetPosition(F,0); if(!EFI_ERROR(S)) S=F->Read(F,&N,Gzip);
    if(EFI_ERROR(S) || N!=2 || Gzip[0]!=0x1F || Gzip[1]!=0x8B) {
      Print(L"[FAIL] initramfs gzip signature (status=%r)\n",S); Failed=TRUE;
    } else Print(L"[OK] initramfs gzip: %Lu bytes\n",I->FileSize);
    CloseInfo(F,I); F=NULL; I=NULL;
  }

  S=OpenInfo(Root,DTB_PATH,&F,&I);
  if(EFI_ERROR(S)) { Print(L"[FAIL] DTB: %r\n",S); Failed=TRUE; }
  else {
    N=sizeof(Fdt); S=F->SetPosition(F,0); if(!EFI_ERROR(S)) S=F->Read(F,&N,Fdt);
    FdtSize=(N>=8)?ReadBe32(Fdt+4):0;
    if(EFI_ERROR(S) || N!=sizeof(Fdt) || ReadBe32(Fdt)!=FDT_MAGIC ||
       FdtSize<40 || FdtSize>I->FileSize) {
      Print(L"[FAIL] FDT header/size (status=%r)\n",S); Failed=TRUE;
    } else Print(L"[OK] DTB FDT: total=%u bytes, file=%Lu bytes\n",FdtSize,I->FileSize);
    CloseInfo(F,I); F=NULL; I=NULL;
  }

  S=OpenInfo(Root,ROOTFS_PATH,&F,&I);
  if(EFI_ERROR(S) || (I->Attribute & EFI_FILE_DIRECTORY)==0) {
    Print(L"[FAIL] RootFS directory: %r\n",S); Failed=TRUE;
  } else Print(L"[OK] RootFS directory accessible: " ROOTFS_PATH L"\n");
  CloseInfo(F,I);

  Root->Close(Root);
  Print(L"\nRESULT: %s\n",Failed?L"CHECK FAILED - aucun transfert effectue":L"CHECK PASSED - aucun transfert effectue");
  return Failed?EFI_COMPROMISED_DATA:EFI_SUCCESS;
}
