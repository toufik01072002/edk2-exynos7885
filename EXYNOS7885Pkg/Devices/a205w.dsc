[Defines]
  PLATFORM_NAME                  = EXYNOS7885Pkg
  PLATFORM_GUID                  = 28f1a3bf-193a-47e3-a7b9-5a435eaab2ee
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010019
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = EXYNOS7885Pkg/EXYNOS7885Pkg.fdf

!include EXYNOS7885Pkg/EXYNOS7885Pkg.dsc

[PcdsFixedAtBuild.common]
  # System Memory - Total usable: ~3004 MiB (3 banks)
  # Bank 0: 0x80000000-0xB8000000 (896 MiB) + 0xBA000000-0xBBC00000 (60 MiB) = 956 MiB
  # Bank 1: 0xC0080000-0x100000000 = 1023.5 MiB
  # Bank 2: 0x880000000-0x8C0000000 = 1024 MiB (above 4GB)
  # Total: ~3004 MiB
  # PcdSystemMemorySize covers first contiguous region (0x80000000-0xB8000000) for SEC stack setup
  # Additional regions added via HOBs in MemoryInitPeiLib
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000
  gArmTokenSpaceGuid.PcdSystemMemorySize|0x38000000
  # UEFI Memory Region for SEC/PEI (stacks + heap) - 48 MiB at 0x80D00000
  gArmPlatformTokenSpaceGuid.PcdSystemMemoryUefiRegionSize|0x03000000
  gEmbeddedTokenSpaceGuid.PcdPrePiStackBase|0x80C00000
  gEmbeddedTokenSpaceGuid.PcdPrePiStackSize|0x00040000      # 256K stack
  gEXYNOS7885PkgTokenSpaceGuid.PcdUefiMemPoolBase|0x80D00000         # DXE Heap base address
  gEXYNOS7885PkgTokenSpaceGuid.PcdUefiMemPoolSize|0x03300000         # UefiMemorySize, DXE heap size
  # Framebuffer (720x1560, 20 MiB at 0xEC000000)
  gEXYNOS7885PkgTokenSpaceGuid.PcdMipiFrameBufferAddress|0x0ec000000
  gEXYNOS7885PkgTokenSpaceGuid.PcdMipiFrameBufferWidth|720
  gEXYNOS7885PkgTokenSpaceGuid.PcdMipiFrameBufferHeight|1560
  gEXYNOS7885PkgTokenSpaceGuid.PcdMipiFrameBufferVisibleWidth|720
  gEXYNOS7885PkgTokenSpaceGuid.PcdMipiFrameBufferVisibleHeight|1560
