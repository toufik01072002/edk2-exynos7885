# État du projet Multiboot EDK2 — Samsung SM-A205W (Exynos 7884)
- **Dépôt GitHub :** toufik01072002/edk2-exynos7885 (branche `feature/kexec-arm64-virtual-boot`)
- **Dossier local Termux :** `~/edk2-exynos7885` et fichiers de test dans `/sdcard/uefi/`
- **Partition boot :** `/dev/block/platform/13500000.dwmmc0/by-name/boot` (sauvegarde intacte dans `/sdcard/uefi/stock_boot.img`)
- **Scripts TWRP prêts :** `/sdcard/uefi/flash_edk2.sh` et `/sdcard/uefi/restore_android.sh`
- **Méthode de boot validée :** S-Boot (`0x10008000`) -> `BootShim.bin` -> copie `EXYNOS7885PKG_UEFI.fd` à `0x90000000` -> démarrage EDK2 réussi (kexec écarté car bloqué par EL2).
- **Correctifs appliqués :**
  1. Limitation de `DeviceMemoryMap.h` sous 4 Go (suppression des banques `0x880000000`) -> bootloop résolu, logo EDK2 affiché.
  2. Alignement écran `720x1560` (`88x80`) dans `ConSplitter.c` et `GraphicsConsole.c`.
  3. Ajout du pilote boutons physiques (`GPA0DAT` à `0x11CB0004` : Vol+ = Haut, Vol- = Bas, Power = Entrée) dans `EXYNOS7885PkgDxe.c`.
- **Prochaine étape :** Pousser sur GitHub, compiler le `.fd`, tester l'affichage net et les boutons dans le menu UEFI, puis configurer le multiboot (Linux / Windows / Android) sur carte microSD.
