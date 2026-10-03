# GPIO interrupts on a three-core SoC (SDK patch 0007)

Symptom (v0.20.0): with the camera open, i.e. CPU1 up, not a single PIR pulse reached the GPIO
interrupt handler on CPU0. With the camera closed every pulse arrived. The button on GPIO 53 was
exposed to the same theft; it was just pressed less often with the camera running.

Cause: every core runs `bk_gpio_driver_init()`, which enables the one shared GPIO interrupt towards
*its own* CPU and installs a `gpio_isr()` that clears every pending status bit, handler or not. The
GPIO interrupt status is a single shared register set, so whichever core serviced the interrupt
first cleared it, and CPU1 (idle apart from the camera) usually won. CPU0's edge-triggered handlers
then never saw the bit.

Fix (`patches/0007-gpio-irq-cpu0-only.patch`): `bk_gpio_driver_init()` registers the GPIO ISR and
enables the interrupt towards the CPU only when built for CPU0. CPU1 and CPU2 keep the GPIO driver
(they still configure pins) but never service the interrupt.

Verification (v0.20.1, camera streaming): every GPIO 24 pulse counted, `pir status` shows
`missed=0`. As a safety net `pir.c` also runs a 50 ms level poller from the reporter task
(`pir_poll()`); edges it catches that the ISR did not are reported as `missed=N`. Since the patch
that counter has stayed at zero.

Lesson for other multi-core Armino projects: audit every `*_driver_init()` that registers an
interrupt for a shared peripheral and make sure exactly one core owns it.
