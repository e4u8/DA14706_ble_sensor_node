/**
 ****************************************************************************************
 *
 * @file main.c
 *
 * @brief BLE Energy Monitor -- DA14706
 *
 *        Merged from:
 *          - BLE-Relay_control project (BLE peripheral, relay GPIO)
 *          - i2c_and_2ch_gpadc project (GPADC 2-channel, AHT20 I2C)
 *
 *        Three FreeRTOS tasks:
 *          ble_peripheral_task  -- BLE GATT server, relay control, measurement notify
 *          gpadc_app_task       -- AC Vrms/Irms/P via GPADC, pushes meas_packet every 1 s
 *          aht20_task           -- temperature/humidity via I2C AHT20, every 2 s
 *
 *        Sleep mode: pm_mode_idle (required for continuous ADC sampling).
 *        BLE stack operates normally in idle mode.
 *
 * Copyright (C) 2015-2022 Dialog Semiconductor.
 *
 ****************************************************************************************
 */

#include <string.h>
#include <stdbool.h>
#include "osal.h"
#include "resmgmt.h"
#include "ad_ble.h"
#include "ad_nvms.h"
#include "ad_gpadc.h"
#include "ad_i2c.h"
#include "ble_mgr.h"
#include "hw_gpio.h"
#include "hw_sys.h"
#include "hw_cpm.h"
#include "sys_clock_mgr.h"
#include "sys_power_mgr.h"
#include "sys_watchdog.h"

#include "platform_devices.h"   /* relay_gpio_cfg, ADC_CH0/1_DEVICE, I2C_DEVICE_MASTER */
#include "meas_packet.h"        /* meas_packet_t, g_meas_queue, MEAS_DATA_NOTIF */

#include "include/gpadc_app.h"
#include "include/aht20_task.h"

/* Task priorities */
#define mainBLE_PERIPHERAL_TASK_PRIORITY    ( OS_TASK_PRIORITY_NORMAL )
#define mainGPADC_TASK_PRIORITY             ( OS_TASK_PRIORITY_NORMAL )

#if dg_configUSE_WDOG
__RETAINED_RW int8_t idle_task_wdog_id = -1;
#endif

/* Shared measurement queue -- depth=1, written by gpadc_app_task,
 * read by ble_peripheral_task. Defined here, extern in meas_packet.h. */
OS_QUEUE g_meas_queue;

/* Task handles */
static OS_TASK handle         = NULL;
static OS_TASK prvGPADCTask_h = NULL;

/* Forward declarations */
static void prvSetupHardware(void);
static void periph_init(void);
OS_TASK_FUNCTION(ble_peripheral_task, params);

/* -----------------------------------------------------------------------
 * System initialisation task
 * ----------------------------------------------------------------------- */
static OS_TASK_FUNCTION(system_init, pvParameters)
{
#if defined CONFIG_RETARGET
        extern void retarget_init(void);
#endif

        /* Clock init */
        cm_sys_clk_init(sysclk_XTAL32M);
        cm_apb_set_clock_divider(apb_div1);
        cm_ahb_set_clock_divider(ahb_div1);
        cm_lp_clk_init();

        /* Watchdog */
        sys_watchdog_init();

#if dg_configUSE_WDOG
        idle_task_wdog_id = sys_watchdog_register(false);
        ASSERT_WARNING(idle_task_wdog_id != -1);
        sys_watchdog_configure_idle_id(idle_task_wdog_id);
#endif

        /* Hardware init (ADC GPIO, I2C IO, relay GPIO via pm_system_init) */
        prvSetupHardware();

        /* Sleep mode: idle is required because gpadc_app_task runs a continuous
         * sampling loop and must not be interrupted by deep sleep.
         * BLE stack operates normally in pm_mode_idle. */
        pm_sleep_mode_set(pm_mode_idle);
        pm_set_sys_wakeup_mode(pm_sys_wakeup_mode_fast);

#if defined CONFIG_RETARGET
        retarget_init();
#endif

        /* Create the measurement queue before any task that uses it */
        g_meas_queue = xQueueCreate(1, sizeof(meas_packet_t));
        OS_ASSERT(g_meas_queue != NULL);

        /* BLE stack init -- must precede ble_peripheral_task creation */
        ble_mgr_init();

        /* --- Task 1: BLE peripheral (relay control + measurement notifications) --- */
        OS_TASK_CREATE("BLE Peripheral",
                       ble_peripheral_task,
                       NULL,
                       200 * OS_STACK_WORD_SIZE,
                       mainBLE_PERIPHERAL_TASK_PRIORITY,
                       handle);
        OS_ASSERT(handle);

        /* --- Task 2: GPADC 2-channel AC measurement --- */
        OS_TASK_CREATE("GPADC",
                       gpadc_app_task,
                       NULL,
                       1024 * OS_STACK_WORD_SIZE,
                       mainGPADC_TASK_PRIORITY,
                       prvGPADCTask_h);
        OS_ASSERT(prvGPADCTask_h != NULL);

        /* --- Task 3: AHT20 I2C temperature/humidity (creates its own task) --- */
        aht20_task_start();

        /* SysInit work done */
        OS_TASK_DELETE(OS_GET_CURRENT_TASK());
}

/* -----------------------------------------------------------------------
 * main()
 * ----------------------------------------------------------------------- */
int main(void)
{
        OS_BASE_TYPE status;

        status = OS_TASK_CREATE("SysInit",
                                system_init,
                                (void *)0,
                                1200,
                                OS_TASK_PRIORITY_HIGHEST,
                                handle);
        OS_ASSERT(status == OS_TASK_CREATE_SUCCESS);

        OS_TASK_SCHEDULER_RUN();

        for (;;);
}

/* -----------------------------------------------------------------------
 * periph_init -- called by pm_system_init(); configures ADC GPIO pins.
 * I2C and relay pins are managed via adapter io_config calls in prvSetupHardware.
 * ----------------------------------------------------------------------- */
static void periph_init(void)
{
#if (DEVICE_FAMILY == DA1470X)
        hw_gpio_configure_pin(HW_GPIO_PORT_0, HW_GPIO_PIN_5,
                              HW_GPIO_MODE_INPUT, HW_GPIO_FUNC_ADC, false);
        hw_gpio_configure_pin(HW_GPIO_PORT_0, HW_GPIO_PIN_6,
                              HW_GPIO_MODE_INPUT, HW_GPIO_FUNC_ADC, false);
#endif
}

/* -----------------------------------------------------------------------
 * prvSetupHardware -- combined hardware init for BLE + GPADC + I2C
 * ----------------------------------------------------------------------- */
static void prvSetupHardware(void)
{
        pm_system_init(periph_init);

        /* Enable COM power domain before accessing GPIO pad latches */
        hw_sys_pd_com_enable();

        /* GPADC IO -- place pins in 'off' state (adapter pattern) */
        ad_gpadc_io_config(ADC_CH0_DEVICE->id, ADC_CH0_DEVICE->io, AD_IO_CONF_OFF);
        ad_gpadc_io_config(ADC_CH1_DEVICE->id, ADC_CH1_DEVICE->io, AD_IO_CONF_OFF);

        /* I2C IO -- place pins in 'off' state (adapter pattern) */
#if dg_configI2C_ADAPTER
        ad_i2c_io_config(((ad_i2c_controller_conf_t *)I2C_DEVICE_MASTER)->id,
                         ((ad_i2c_controller_conf_t *)I2C_DEVICE_MASTER)->io,
                         AD_IO_CONF_OFF);
#endif

        hw_sys_pd_com_disable();
}

/* -----------------------------------------------------------------------
 * FreeRTOS hook functions
 * ----------------------------------------------------------------------- */
OS_APP_MALLOC_FAILED(void)
{
        ASSERT_ERROR(0);
}

OS_APP_IDLE(void)
{
#if dg_configUSE_WDOG
        sys_watchdog_notify(idle_task_wdog_id);
#endif
}

OS_APP_STACK_OVERFLOW(OS_TASK pxTask, char *pcTaskName)
{
        (void)pcTaskName;
        (void)pxTask;
        ASSERT_ERROR(0);
}

OS_APP_TICK(void)
{
}
