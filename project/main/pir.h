/* PIR motion sensor: GPIO 24 output (ISR), GPIO 50 enable, GPIO 25/26 sensitivity.
 * CPU0 only (GPIO ISR + KV). See TESTPLAN-motion.md and the stock notes in pir.c. */
#ifndef DU7_PIR_H
#define DU7_PIR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Result of one PIR edge (hardware or simulated). */
enum pir_edge_result {
	PIR_EDGE_ACCEPTED = 0,   /* counted and the motion callback ran */
	PIR_EDGE_OFF,            /* PIR disabled (`pir off`) */
	PIR_EDGE_WARMUP,         /* within the warm-up blanking after enable/boot */
	PIR_EDGE_COOLDOWN,       /* within the cooldown after the last accepted motion */
};

typedef void (*pir_motion_cb_t)(void);

/* Load KV settings, configure the pins, register the GPIO 24 ISR. `cb` runs in ISR context on
 * every accepted motion edge: keep it to flag/counter updates. */
void pir_init(pir_motion_cb_t cb);

bool pir_is_on(void);
uint32_t pir_sens_get(void);      /* 0..3 */
uint32_t pir_cooldown_ms(void);
uint32_t pir_count(void);         /* accepted motion edges since boot (hardware + simulated) */

/* Simulated PIR edge through the same handler as the ISR (`motion test`). */
enum pir_edge_result pir_trigger(void);

/* Shell commands. Output text goes to `out` (NUL-terminated, at most `cap` bytes); the caller
 * prints/REPLYs it. `pir on|off | sens 0-3 | cooldown MS | status`, `motion test`. */
void pir_command(int argc, char **argv, char *out, size_t cap);
void motion_command(int argc, char **argv, char *out, size_t cap);

#endif
