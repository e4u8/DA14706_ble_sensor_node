/*
 * meas_packet.h
 *
 * Shared measurement packet definition for the BLE Energy Monitor node.
 *
 * Produced every 1 s by gpadc_app_task, consumed by ble_peripheral_task
 * which injects relay_state and sends a GATT notification.
 *
 * S, Q and PF are intentionally absent: the Central Node derives them from
 * v_rms, i_rms and p_w, so they do not need to be transmitted.
 *
 * The queue (depth=1, xQueueOverwrite) is created in main.c before any
 * task runs so both tasks can reference it safely at start-up.
 */

#ifndef MEAS_PACKET_H_
#define MEAS_PACKET_H_

#include <stdint.h>
#include "osal.h"   /* OS_QUEUE, OS_TASK */

/* -------------------------------------------------------------------------
 * Measurement packet — 15 bytes, little-endian on CM33
 * ------------------------------------------------------------------------- */
typedef struct __attribute__((packed)) {
        int16_t  v_rms;       /* centivolts      — 23045  = 230.45 V  */
        int16_t  i_rms;       /* milliamps       —  1500  =   1.500 A */
        int32_t  p_w;         /* centiwatts      — 12345  = 123.45 W  */
        int16_t  freq;        /* centi-Hz        —  5000  =  50.00 Hz (ZCD placeholder) */
        int16_t  temp;        /* centi-°C        —  2150  =  21.50 °C */
        uint16_t humid;       /* centi-%RH       —  6000  =  60.00 %  */
        uint8_t  relay_state; /* 0 = OFF, 1 = ON — filled by ble_peripheral_task */
} meas_packet_t;              /* 2+2+4+2+2+2+1 = 15 bytes */

/* Depth-1 queue: written by gpadc_app_task (xQueueOverwrite),
 * read by ble_peripheral_task (xQueueReceive non-blocking).
 * Defined in main.c. */
extern OS_QUEUE g_meas_queue;

/* BLE peripheral task handle — assigned at task start-up.
 * gpadc_app_task sends MEAS_DATA_NOTIF on it after each queue write.
 * Defined in ble_peripheral_task.c. */
extern OS_TASK g_ble_peripheral_task_handle;

/* FreeRTOS task-notification bit for new measurement data (bit 4).
 * Bits 0 (BLE events), 2 (CTS), 3 (BCS) are already taken. */
#define MEAS_DATA_NOTIF  (1 << 4)

#endif /* MEAS_PACKET_H_ */
