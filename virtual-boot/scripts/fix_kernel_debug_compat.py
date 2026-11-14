#!/usr/bin/env python3
"""Apply small compatibility fixes required by the SM-A205W virtual-boot kernel build."""
from pathlib import Path


def replace_once(path: str, old: str, new: str, description: str) -> None:
    file = Path(path)
    source = file.read_text()
    if new in source:
        print(f"Already applied: {description}")
        return
    if old not in source:
        raise SystemExit(f"Unexpected source layout while applying: {description}")
    file.write_text(source.replace(old, new, 1))
    print(f"Applied: {description}")


replace_once(
    "kernel-src/drivers/trace/exynos-ss.c",
    "\t\tif (sec_debug_get_debug_level() &&\n"
    "\t\t\t(__raw_readl(exynos_ss_get_base_vaddr() + ESS_OFFSET_SCRATCH)\n"
    "\t\t\t\t\t== ESS_SIGN_SCRATCH) &&\n"
    "\t\t\tsec_debug_enter_upload())\n"
    "\t\t\texynos_sdm_dump_secure_region();",
    "#ifdef CONFIG_SEC_DEBUG\n"
    "\t\tif (sec_debug_get_debug_level() &&\n"
    "\t\t\t(__raw_readl(exynos_ss_get_base_vaddr() + ESS_OFFSET_SCRATCH)\n"
    "\t\t\t\t\t== ESS_SIGN_SCRATCH) &&\n"
    "\t\t\tsec_debug_enter_upload())\n"
    "\t\t\texynos_sdm_dump_secure_region();\n"
    "#endif /* CONFIG_SEC_DEBUG */",
    "guard Exynos secure-upload hooks with CONFIG_SEC_DEBUG",
)

replace_once(
    "kernel-src/drivers/usb/host/ehci-hub.c",
    'ehci_vdbg (ehci, "port %d reset\\n", wIndex + 1);',
    'ehci_dbg (ehci, "port %d reset\\n", wIndex + 1);',
    "replace unavailable ehci_vdbg with ehci_dbg",
)
