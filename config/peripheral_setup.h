/**
 ****************************************************************************************
 *
 * @file peripheral_setup.h
 *
 * @brief Pin definitions for I2C (AHT20) — MikroBUS 1 (P1_11 SDA, P1_12 SCL)
 *        ADC pin definitions live directly in platform_devices.c.
 *
 ****************************************************************************************
 */

#ifndef CONFIG_PERIPHERAL_SETUP_H_
#define CONFIG_PERIPHERAL_SETUP_H_

#include "hw_gpio.h"

/* I2C pin assignment — MikroBUS 1 */
#define I2C_MASTER_SCL_PORT     ( HW_GPIO_PORT_1 )
#define I2C_MASTER_SCL_PIN      ( HW_GPIO_PIN_12 )

#define I2C_MASTER_SDA_PORT     ( HW_GPIO_PORT_1 )
#define I2C_MASTER_SDA_PIN      ( HW_GPIO_PIN_11 )

/* AHT20 I2C address (7-bit) */
#define I2C_ADDR_AHT20          ( 0x38u )
#define I2C_SLAVE_ADDRESS       ( I2C_ADDR_AHT20 )

#endif /* CONFIG_PERIPHERAL_SETUP_H_ */
