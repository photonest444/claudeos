/* Programmable Interval Timer, channel 0 -> IRQ0 at TIMER_HZ (1 kHz).
 * Each tick drives the scheduler (sleep wake-ups and time slicing). */
#include "../include/drivers.h"
#include "../include/io.h"
#include "../include/arch.h"
#include "../include/task.h"

static volatile uint32_t ticks;

static void timer_irq(regs_t *r) {
    UNUSED(r);
    ticks++;
    scheduler_tick();
}

void pit_init(void) {
    uint32_t div = 1193182 / TIMER_HZ;
    outb(0x43, 0x36);                 /* channel 0, lo/hi byte, mode 3 */
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);
    irq_install(0, timer_irq);
}

uint32_t timer_ticks(void) { return ticks; }
