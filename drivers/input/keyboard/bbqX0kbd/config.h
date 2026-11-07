/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Keyboard Driver for Blackberry Keyboards BBQ10 from arturo182. Software written by wallComputer.
 * config.h: File to store driver configurations.
 */

#ifndef CONFIG_H_
#define CONFIG_H_

#define BBQX0KBD_DEFAULT_I2C_ADDRESS	0x1F
#define BBQX0KBD_ASSIGNED_I2C_ADDRESS BBQX0KBD_DEFAULT_I2C_ADDRESS

#define BBQ20KBD_TRACKPAD_AS_MOUSE		0
#define BBQ20KBD_TRACKPAD_AS_KEYS		1
#define BBQ20KBD_TRACKPAD_USE BBQ20KBD_TRACKPAD_AS_KEYS

#define BBQ20KBD_TRACKPAD_KEYS_OFFSET 26


#endif
