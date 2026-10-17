/* SPDX-License-Identifier: Apache-2.0 */
/* freebell-bk7258 board profile: Tuya "Smart Visual Doorbell", board IP108_MAIN_V05 (Tuya product IP10B).
 *
 * Every board-specific fact the firmware uses: BK7258 GPIO ids, ADC channels, divider ratio. Sources
 * (stock firmware disassembly + bench measurements) are in docs/notes/pin-mapping.md and the module
 * notes named below. Included through board.h only; a new board gets its own file next to this one. */
#ifndef BOARD_IP108_MAIN_V05_H
#define BOARD_IP108_MAIN_V05_H

#define BOARD_NAME "ip108_main_v05"

/* ---- inputs */
#define BOARD_PIN_BUTTON       53   /* doorbell button, active low with an external pull-up. The same line keys the
                                     * on-board 433 MHz chime transmitter: driving it low rings every paired chime */
#define BOARD_PIN_KEY2         51   /* the stock firmware's second key (long press); not wired here, parked as input + pull-up */
#define BOARD_PIN_UART_RX      10   /* console RX, in the SDK's static wake map (falling edge) */

/* ---- PIR (docs/notes/pir.md): output pulses 2-4 s, enable and two sensitivity bits (0 = least sensitive) */
#define BOARD_PIN_PIR_OUT      24   /* active-high output, rising-edge interrupt */
#define BOARD_PIN_PIR_EN       50   /* high = enabled */
#define BOARD_PIN_PIR_SENS0    25   /* sensitivity bit 0 (the stock's "pir set1") */
#define BOARD_PIN_PIR_SENS1    26   /* sensitivity bit 1 (the stock's "pir set0") */

/* ---- lights (docs/notes/night-light.md), active high */
#define BOARD_PIN_LED_WHITE    44   /* white fill light (no IR emitter on this board) */
#define BOARD_PIN_LED_RING     45   /* the four blue button-ring LEDs */

/* ---- audio (docs/notes/audio.md) */
#define BOARD_PIN_AMP_EN       46   /* speaker amplifier enable, active high; floats at boot */

/* ---- camera (docs/notes/camera.md): OmniVision OV9650 on DVP, SCCB on I2C1 GPIO 0/1 (set in the AVDK config) */
#define BOARD_PIN_CAM_PWR      28   /* sensor supply; also feeds the light sensor divider and the microSD slot */
#define BOARD_PIN_CAM_LDO       5   /* camera LDO enable */
#define BOARD_PIN_CAM_PWDN     52   /* sensor PWDN/RESET: the stock pulses it low 100 ms / high 100 ms / low */
#define BOARD_PIN_IRCUT_DAY    54   /* IR-cut coil, one pulse per scene change */
#define BOARD_PIN_IRCUT_NIGHT  55
#define BOARD_PIN_SENSOR_RAIL   9   /* light sensor / LED rail, kept high while awake */

/* ---- power (docs/notes/power.md) */
#define BOARD_PIN_USB_PRESENT  19   /* 1 = USB plugged */
#define BOARD_PIN_CHARGING     15   /* charger status */

/* ---- microSD (docs/notes/sdcard.md): powered from BOARD_PIN_CAM_PWR */
#define BOARD_PIN_SD_DETECT     6   /* card detect, input with pull-up, 0 = card present */

/* ---- analog */
#define BOARD_ADC_LIGHT        14   /* light sensor (LDR on GPIO 12); higher mV = darker; needs BOARD_PIN_CAM_PWR */
#define BOARD_ADC_SUPPLY        9   /* regulated ~3.07 V supply sense (GPIO 7), reported as "mv" (sys info "batt") */
#define BOARD_ADC_BATTERY      15   /* battery node, on BOARD_PIN_BATTERY */
#define BOARD_PIN_BATTERY      13   /* GPIO behind BOARD_ADC_BATTERY (ADC only: not the microSD power) */
#define BOARD_BATT_RATIO_PCT  200   /* battery divider: cell mV = ADC mV x 2.00 */

#endif
