#!/usr/bin/env python3
"""Apply the reviewed A20 ARM64 kexec DTB-register patch to a clean kernel checkout."""
import argparse
from pathlib import Path


def replace_once(source, old, new, label):
    count = source.count(old)
    if count != 1:
        raise SystemExit(f"BLOCKED: expected one {label} anchor, found {count}")
    return source.replace(old, new, 1)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel-source", required=True, type=Path)
    args = parser.parse_args()

    machine_path = args.kernel_source / "arch/arm64/kernel/machine_kexec.c"
    relocate_path = args.kernel_source / "arch/arm64/kernel/relocate_kernel.S"
    if not machine_path.is_file() or not relocate_path.is_file():
        raise SystemExit("BLOCKED: expected ARM64 kexec source files are missing")

    machine = machine_path.read_text(encoding="utf-8")
    relocate = relocate_path.read_text(encoding="utf-8")

    markers_present = (
        "static phys_addr_t kimage_dtb;" in machine,
        "kimage_dtb = dtb;" in machine,
        "kimage_start, kimage_dtb);" in machine,
        "mov\tx19, x2" in relocate,
        "mov\tx0, x19" in relocate,
    )
    if all(markers_present):
        print("A20 kexec DTB handoff patch already present.")
        return
    if any(markers_present):
        raise SystemExit("BLOCKED: partial A20 kexec DTB handoff patch detected")

    machine = replace_once(
        machine,
        "#include <linux/slab.h>\n",
        "#include <linux/slab.h>\n#include <linux/string.h>\n",
        "string header include",
    )
    machine = replace_once(
        machine,
        "static unsigned long kimage_start;\n",
        "static unsigned long kimage_start;\nstatic phys_addr_t kimage_dtb;\n",
        "DTB state declaration",
    )
    machine = replace_once(
        machine,
        "void machine_kexec_cleanup(struct kimage *kimage)\n{\n\t/* Empty routine needed to avoid build errors. */\n}",
        "void machine_kexec_cleanup(struct kimage *kimage)\n{\n\tkimage_start = 0;\n\tkimage_dtb = 0;\n}",
        "kexec cleanup function",
    )
    machine = replace_once(
        machine,
        "int machine_kexec_prepare(struct kimage *kimage)\n{\n\tkimage_start = kimage->start;\n\tkexec_image_info(kimage);\n\n\treturn 0;\n}",
        """int machine_kexec_prepare(struct kimage *kimage)
{
\tunsigned int i;
\tphys_addr_t dtb = 0;

\tkimage_start = kimage->start;
\tkimage_dtb = 0;

\t/*
\t * A normal ARM64 kexec image must carry exactly one FDT segment.
\t * Pass its physical destination to the relocation trampoline so the
\t * target receives the Linux ARM64 boot ABI in x0.
\t */
\tif (kimage->type == KEXEC_TYPE_DEFAULT) {
\t\tfor (i = 0; i < kimage->nr_segments; i++) {
\t\t\tconst struct kexec_segment *segment = &kimage->segment[i];
\t\t\tbool is_dtb = false;

\t\t\tif (segment->bufsz < sizeof(__be32) ||
\t\t\t    segment->memsz < sizeof(__be32))
\t\t\t\tcontinue;

\t\t\tif (kimage->file_mode) {
\t\t\t\t__be32 magic;

\t\t\t\tmemcpy(&magic, segment->kbuf, sizeof(magic));
\t\t\t\tis_dtb = fdt32_to_cpu(magic) == OF_DT_HEADER;
\t\t\t} else {
\t\t\t\tis_dtb = kexec_is_dtb(segment->buf);
\t\t\t}

\t\t\tif (!is_dtb)
\t\t\t\tcontinue;

\t\t\tif (dtb) {
\t\t\t\tpr_err("kexec: multiple DTB segments are not supported\\\\n");
\t\t\t\treturn -EINVAL;
\t\t\t}

\t\t\tif (!segment->mem || (segment->mem & 7)) {
\t\t\t\tpr_err("kexec: invalid DTB destination address\\\\n");
\t\t\t\treturn -EINVAL;
\t\t\t}

\t\t\tdtb = segment->mem;
\t\t}

\t\tif (!dtb) {
\t\t\tpr_err("kexec: refusing ARM64 image without a DTB segment\\\\n");
\t\t\treturn -EINVAL;
\t\t}

\t\tkimage_dtb = dtb;
\t\tpr_info("kexec: ARM64 target DTB at %pa\\\\n", &kimage_dtb);
\t}

\tkexec_image_info(kimage);
\treturn 0;
}""",
        "kexec prepare function",
    )
    machine = replace_once(
        machine,
        "reboot_code_buffer_phys, kimage->head, kimage_start, 0);",
        "reboot_code_buffer_phys, kimage->head, kimage_start, kimage_dtb);",
        "soft restart DTB argument",
    )

    relocate = replace_once(
        relocate,
        "\tmov\tx18, x1\t\t\t\t/* x18 = kimage_start */\n",
        "\tmov\tx18, x1\t\t\t\t/* x18 = kimage_start */\n\tmov\tx19, x2\t\t\t\t/* x19 = physical DTB address */\n",
        "relocation DTB preservation",
    )
    relocate = replace_once(
        relocate,
        "\tmov\tx0, xzr\n\tmov\tx1, xzr\n\tmov\tx2, xzr\n\tmov\tx3, xzr\n\tbr\tx18",
        "\tmov\tx0, x19\t\t\t\t/* x0 = physical DTB address */\n\tmov\tx1, xzr\n\tmov\tx2, xzr\n\tmov\tx3, xzr\n\tbr\tx18",
        "final ARM64 entry registers",
    )

    machine_path.write_text(machine, encoding="utf-8")
    relocate_path.write_text(relocate, encoding="utf-8")
    print("Applied A20 kexec DTB handoff ABI v1.")
    print("Normal kexec now requires one DTB segment and passes its physical address in x0.")
    print("Crash-kexec behavior remains unchanged; EDK2 handoff is not implemented.")


if __name__ == "__main__":
    main()
