#!/bin/sh
#
# Move the heavy device interrupts off CPU0.
#
# Measured before this existed (32 MB over the USB gadget while writing to
# UFS): CPU0 carried every device interrupt on the machine - musb, ufshcd,
# panfrost, rdma, i2c, dsi, wlan and btif - on top of the timer softirq. A
# single 32 MB transfer produced 206,356 musb interrupts, all on CPU0, and the
# machine then locked up hard with
#
#   watchdog: BUG: soft lockup - CPU#0 stuck for 49s! [swapper/0:0]
#   pc : mix_interrupt_randomness+0x0/0x124
#   lr : call_timer_fn.isra.0+0x24/0x80
#
# and no panic, no hung task and nothing in pstore, because the lockup detector
# itself needs timer interrupts. The blackbox heartbeat file kept being written
# once a second right up to the freeze, which is what proved the kernel was
# alive (a hard lockup) rather than dead.
#
# Spreading them costs nothing and leaves CPU0 with only the timer and IPIs.

IRQ=/proc/interrupts

spread() {
    name="$1"; mask="$2"
    line=$(grep -E "$name" "$IRQ" 2>/dev/null | head -1)
    [ -z "$line" ] && return
    n=$(echo "$line" | cut -d: -f1 | tr -d ' ')
    [ -w "/proc/irq/$n/smp_affinity" ] && echo "$mask" > "/proc/irq/$n/smp_affinity" 2>/dev/null
}

spread 'musb'           '04'
spread 'ufshcd'         '08'
spread 'panfrost-job'   '10'
spread 'panfrost-mmu'   '10'
spread 'panfrost-gpu'   '10'
spread '14007000.rdma'  '20'
spread '11e01000.i2c'   '20'
spread '14013000.dsi'   '20'
spread 'wlan0'          '40'
spread 'btif'           '80'
