/**
 ****************************************************************************************
 *
 * @file platform_devices.h
 *
 * @brief Merged platform devices -- relay GPIO (BLE project) +
 *        GPADC channel handles + I2C master handle (ADC+I2C project).
 *
 ****************************************************************************************
 */

#ifndef PLATFORM_DEVICES_H_
#define PLATFORM_DEVICES_H_

#include "ad.h"

/* -----------------------------------------------------------------------
 * Relay -- MikroBUS 2, PWM pin = P1_00
 * Used by ble_peripheral_task.c via ad_io_configure().
 * ----------------------------------------------------------------------- */
#define RELAY_PORT    (HW_GPIO_PORT_1)
#define RELAY_PIN     (HW_GPIO_PIN_0)

static const ad_io_conf_t relay_gpio_cfg[] = {
    {
        .port = RELAY_PORT,
        .pin  = RELAY_PIN,
        .on = {
            .mode     = HW_GPIO_MODE_OUTPUT,
            .function = HW_GPIO_FUNC_GPIO,
            .high     = true    /* relay ON  = pin HIGH */
        },
        .off = {
            .mode     = HW_GPIO_MODE_OUTPUT,
            .function = HW_GPIO_FUNC_GPIO,
            .high     = false   /* relay OFF = pin LOW  */
        },
    }
};

/* -----------------------------------------------------------------------
 * GPADC -- two single-ended channels
 *   CH0 = current  (LEM HLSR 10P Hall sensor), GPIO P0_5
 *   CH1 = voltage  (BEL DPC12 transformer),    GPIO P0_6
 * Definitions live in config/platform_devices.c.
 * ----------------------------------------------------------------------- */
#ifndef __CONST
#define __CONST const
#endif

typedef __CONST void* PERIPHERAL_DEVICE;

#include "ad_gpadc.h"
typedef __CONST ad_gpadc_controller_conf_t* gpadc_device;

#if dg_configGPADC_ADAPTER || dg_configUSE_HW_GPADC
extern gpadc_device ADC_CH0_DEVICE;
extern gpadc_device ADC_CH1_DEVICE;
#endif

/* -----------------------------------------------------------------------
 * I2C master -- AHT20 temperature/humidity sensor on MikroBUS 1
 * (P1_11 = SDA, P1_12 = SCL -- see config/peripheral_setup.h)
 * Definition lives in config/platform_devices.c.
 * ----------------------------------------------------------------------- */
#if dg_configI2C_ADAPTER
extern PERIPHERAL_DEVICE I2C_DEVICE_MASTER;
#endif

#endif /* PLATFORM_DEVICES_H_ */
