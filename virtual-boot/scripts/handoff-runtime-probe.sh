#!/bin/busybox sh
# Runtime memory-map observation only. Never assigns or reserves physical RAM.
OUT=/run/handoff-probe.txt
{
    echo "SM-A205W HANDOFF RUNTIME PROBE"
    echo "state=DISABLED"
    echo "purpose=observation_only"
    echo "descriptor_transport=UNASSIGNED"
    echo "descriptor_address=UNASSIGNED"
    echo "physical_reservations=UNVERIFIED"
    echo "register_contract=UNVERIFIED"
    echo "el_mmu_cache_state=UNVERIFIED"
    echo "dma_quiescence=UNVERIFIED"
    echo "secondary_cpu_state=UNVERIFIED"
    echo
    echo "[iomem: observed entries only]"
    if [ -r /proc/iomem ]; then
        grep -E "System RAM|reserved|uefi|tima|seclog|cma|framebuffer|modem|gnss|wifi|abox|bootshim" /proc/iomem
    else
        echo "UNAVAILABLE"
    fi
    echo
    echo "[device-tree reserved-memory: observed nodes only]"
    if [ -d /proc/device-tree/reserved-memory ]; then
        for node in /proc/device-tree/reserved-memory/*; do
            [ -d "$node" ] || continue
            echo "node=$node"
            if [ -r "$node/reg" ]; then
                printf "reg="
                hexdump -v -e '1/1 "%02x"' "$node/reg"
                echo
            else
                echo "reg=UNAVAILABLE"
            fi
            if [ -e "$node/no-map" ]; then echo "no-map=yes"; fi
            if [ -e "$node/reusable" ]; then echo "reusable=yes"; fi
        done
    else
        echo "UNAVAILABLE"
    fi
    echo
    echo "RESULT: observational data only; no reservation or handoff authorization."
} > "$OUT"
cat "$OUT"
