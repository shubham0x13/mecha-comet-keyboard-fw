/*
 * Copyright (c) 2026 Mecha Systems
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * ZMK Studio builds contain every behavior. Leave out the ones that need
 * hardware this keyboard does not have, so they are not offered in Studio.
 */
#define ZMK_BEHAVIORS_OMIT_BT
#define ZMK_BEHAVIORS_OMIT_OUT
#define ZMK_BEHAVIORS_OMIT_BL
#define ZMK_BEHAVIORS_OMIT_SOFT_OFF
