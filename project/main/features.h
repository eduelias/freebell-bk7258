/* SPDX-License-Identifier: Apache-2.0 */
/* freebell-bk7258: build-time feature switches (override with -D... on the compiler command line). */
#ifndef DOORBELL_FEATURES_H
#define DOORBELL_FEATURES_H

/* Board bring-up tools (bringup.c): `mem` raw reads and `cam probe` (camera rails, I2C scans, power scan,
 * bit-banged I2C). Needed when mapping a new board, not in daily use. */
#ifndef DOORBELL_BRINGUP
#define DOORBELL_BRINGUP 0
#endif

#endif
