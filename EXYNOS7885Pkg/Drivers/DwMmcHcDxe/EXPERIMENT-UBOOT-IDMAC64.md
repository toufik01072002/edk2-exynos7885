# Experimental U-Boot 64-bit IDMAC port

Status: source study / isolated port preparation. Not integrated, not built, not hardware-tested.

Base: commit e0f5cfeda003ef4c0edf1577d4ac7e9812449ea5 on port/dwmmc-samsung-idmac.

Upstream source (u-boot/u-boot, main):
- drivers/mmc/dw_mmc.c

Findings:
- U-Boot defines an eight-word (32-byte) IDMAC descriptor for 64-bit addressing: DES0 control, DES1 reserved, DES2 buffer sizes, DES3 reserved, DES4/DES5 buffer address low/high, DES6/DES7 next descriptor low/high.
- It detects 64-bit IDMAC capability from HCON and uses the 64-bit register layout for DBADDR, IDSTS/IDINTEN, DSCADDR and BUFADDR.
- dwmci_prepare_data() resets FIFO, clears IDMAC status, writes DBADDR low/high, prepares descriptors, then enables CTRL.IDMAC_EN|CTRL.DMA_EN, and only then enables BMOD.IDMAC_EN|BMOD.FB.
- dwmci_dma_transfer() waits for RI on reads or TI on writes, clears IDMAC status, disables CTRL.DMA_EN, and releases the bounce buffer.
- The current EDK2 implementation already has the 64-bit descriptor layout, low/high DBADDR writes, and HCON[27] check. The main sequencing point worth isolating is U-Boot's explicit CTRL-before-BMOD ordering, plus a review of IDINTEN and completion/error handling.

Port plan:
1. Build a self-contained experimental IDMAC transaction path using EDK2 MMIO, cache, timer, and allocation services; do not import U-Boot APIs.
2. Preserve the existing bounce-buffer and 64-bit address safeguards; validate descriptor flags and the 13-bit buffer-size field against the exact Exynos implementation before changing them.
3. Compare the current IDINTEN mask (RI|TI) with U-Boot's interrupt enable/clear policy; do not assume generic bit meanings for undocumented Exynos status bits.
4. Keep this path selectable and mutually exclusive with the reference path on each controller.
5. Require a successful GitHub Actions EDK2 build before any user-side test.

License: upstream U-Boot is GPL-2.0+. Treat any copied code accordingly; prefer a clean-room adaptation of the documented sequence and data layout.
