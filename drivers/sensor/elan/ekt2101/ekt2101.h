/*
 * Copyright (c) 2018 Philémon Jaermann
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SENSOR_EKT2101_
#define __SENSOR_EKT2101_

#include <zephyr/drivers/i2c.h>

#define LSM303_DLHC_X_EN_BIT	BIT(0)
#define EKT2101_Y_EN_BIT	BIT(1)
#define EKT2101_Z_EN_BIT	BIT(2)
#define EKT2101_EN_BITS		(LSM303_DLHC_X_EN_BIT | \
					EKT2101_Y_EN_BIT | \
					EKT2101_Z_EN_BIT)

#if	(CONFIG_EKT2101_ODR == 0)
	#define EKT2101_DRDY_WAIT_TIME	134
#elif	(CONFIG_EKT2101_ODR == 1)
	#define EKT2101_DRDY_WAIT_TIME	67
#elif	(CONFIG_EKT2101_ODR == 2)
	#define EKT2101_DRDY_WAIT_TIME	34
#elif	(CONFIG_EKT2101_ODR == 3)
	#define EKT2101_DRDY_WAIT_TIME	14
#elif	(CONFIG_EKT2101_ODR == 4)
	#define EKT2101_DRDY_WAIT_TIME	7
#elif	(CONFIG_EKT2101_ODR == 5)
	#define EKT2101_DRDY_WAIT_TIME	4
#elif	(CONFIG_EKT2101_ODR == 6)
	#define EKT2101_DRDY_WAIT_TIME	2
#elif	(CONFIG_EKT2101_ODR == 7)
	#define EKT2101_DRDY_WAIT_TIME	1
#endif

#define EKT2101_ODR_SHIFT	2
#define EKT2101_ODR_BITS	(CONFIG_EKT2101_ODR << \
					EKT2101_ODR_SHIFT)

#if	(CONFIG_EKT2101_RANGE == 1)
	#define EKT2101_LSB_GAUSS_XY    1100
	#define EKT2101_LSB_GAUSS_Z     980
#elif	(CONFIG_EKT2101_RANGE == 2)
	#define EKT2101_LSB_GAUSS_XY    855
	#define EKT2101_LSB_GAUSS_Z     760
#elif	(CONFIG_EKT2101_RANGE == 3)
	#define EKT2101_LSB_GAUSS_XY    670
	#define EKT2101_LSB_GAUSS_Z     600
#elif	(CONFIG_EKT2101_RANGE == 4)
	#define EKT2101_LSB_GAUSS_XY    450
	#define EKT2101_LSB_GAUSS_Z     400
#elif	(CONFIG_EKT2101_RANGE == 5)
	#define EKT2101_LSB_GAUSS_XY    400
	#define EKT2101_LSB_GAUSS_Z     355
#elif	(CONFIG_EKT2101_RANGE == 6)
	#define EKT2101_LSB_GAUSS_XY    330
	#define EKT2101_LSB_GAUSS_Z     295
#elif	(CONFIG_EKT2101_RANGE == 7)
	#define EKT2101_LSB_GAUSS_XY    230
	#define EKT2101_LSB_GAUSS_Z     205
#endif

#define EKT2101_FS_SHIFT	5
#define EKT2101_FS_BITS		(CONFIG_EKT2101_RANGE << \
					EKT2101_FS_SHIFT)
#define EKT2101_CONT_UPDATE	0x00
#define EKT2101_DRDY		BIT(0)

#define EKT2101_CRA_REG_M		0x00
#define EKT2101_CRB_REG_M		0x01
#define EKT2101_MR_REG_M		0x02
#define EKT2101_REG_X_LSB	0x03
#define EKT2101_SR_REG_M		0x09

struct lsm303dlhc_data {
	int16_t magn_x;
	int16_t magn_y;
	int16_t magn_z;
};

struct lsm303dlhc_config {
	struct i2c_dt_spec i2c;
};
#endif /* _SENSOR_EKT2101_ */
