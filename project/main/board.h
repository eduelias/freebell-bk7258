/* SPDX-License-Identifier: Apache-2.0 */
/* freebell-bk7258: board selection. Every pin, ADC channel and divider ratio the firmware uses comes from the
 * board profile included here (boards/<board>.h); modules never hard-code a GPIO number.
 *
 * Select another board at build time with -DDOORBELL_BOARD_HEADER='"boards/<board>.h"'. A profile must define
 * every BOARD_* name below; pins a board does not have are -1 where the module supports that (see the profile). */
#ifndef DOORBELL_BOARD_H
#define DOORBELL_BOARD_H

#ifndef DOORBELL_BOARD_HEADER
#define DOORBELL_BOARD_HEADER "boards/ip108_main_v05.h"
#endif
#include DOORBELL_BOARD_HEADER

#define BOARD_STR_(x) #x
#define BOARD_STR(x) BOARD_STR_(x)   /* pin number as a string literal, for fixed log/probe text */

#if !defined(BOARD_NAME) || !defined(BOARD_PIN_BUTTON) || !defined(BOARD_PIN_KEY2) || !defined(BOARD_PIN_UART_RX) || \
    !defined(BOARD_PIN_PIR_OUT) || !defined(BOARD_PIN_PIR_EN) || !defined(BOARD_PIN_PIR_SENS0) || !defined(BOARD_PIN_PIR_SENS1) || \
    !defined(BOARD_PIN_LED_WHITE) || !defined(BOARD_PIN_LED_RING) || !defined(BOARD_PIN_AMP_EN) || \
    !defined(BOARD_PIN_CAM_PWR) || !defined(BOARD_PIN_CAM_LDO) || !defined(BOARD_PIN_CAM_PWDN) || \
    !defined(BOARD_PIN_IRCUT_DAY) || !defined(BOARD_PIN_IRCUT_NIGHT) || !defined(BOARD_PIN_SENSOR_RAIL) || \
    !defined(BOARD_PIN_USB_PRESENT) || !defined(BOARD_PIN_CHARGING) || !defined(BOARD_PIN_SD_DETECT) || \
    !defined(BOARD_ADC_LIGHT) || !defined(BOARD_ADC_BATTERY) || !defined(BOARD_PIN_BATTERY) || !defined(BOARD_BATT_RATIO_PCT)
#error "board profile incomplete: see board.h for the required BOARD_* names"
#endif

#endif
