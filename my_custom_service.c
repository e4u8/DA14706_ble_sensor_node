/**
 ****************************************************************************************
 *
 * @file my_custom_service.c
 *
 * @brief BLE custom service -- two characteristics:
 *
 *   Char 1  UUID 11111111-0000-0000-0000-111111111111  R/W/Notify  1 byte
 *           Relay control: 0x01=ON, 0x00=OFF, 0xFF=TOGGLE
 *           Descriptors: CCC (0x2902) + CUD (0x2901)
 *
 *   Char 2  UUID 22222222-0000-0000-0000-222222222222  Notify only  15 bytes
 *           Measurement packet: Vrms, Irms, P, freq, temp, humid, relay_state
 *           Descriptor: CCC (0x2902)
 *
 *   Service UUID: 00000000-1111-2222-2222-333333333333
 *   ble_gatts_get_num_attr(0, 2, 3) = 1 + 0 + 4 + 3 = 8 attributes total
 *
 * Copyright (C) 2015-2022 Dialog Semiconductor.
 *
 ****************************************************************************************
 */

#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "osal.h"
#include "ble_att.h"
#include "ble_bufops.h"
#include "ble_common.h"
#include "ble_gatt.h"
#include "ble_gatts.h"
#include "ble_storage.h"
#include "ble_uuid.h"
#include "my_custom_service.h"


#define UUID_GATT_CLIENT_CHAR_CONFIGURATION (0x2902)

static const char char_user_descriptor_val[] = "Relay Control: 0x01=ON 0x00=OFF 0xFF=TOGGLE";

/* Forward declarations */
static void mcs_notify_char_value(ble_service_t *svc, uint16_t conn_idx, const uint8_t *value);
static void mcs_notify_meas(ble_service_t *svc, uint16_t conn_idx, const meas_packet_t *pkt);

/* Service context */
typedef struct {
        ble_service_t svc;

        /* Application callbacks */
        const my_custom_service_cb_t *cb;

        /* Char 1: relay control (R/W/Notify, 1 byte) */
        uint16_t mc_char_value_h;
        uint16_t mc_char_value_ccc_h;

        /* Char 2: measurement notifications (Notify only, 15 bytes) */
        uint16_t mc_meas_value_h;
        uint16_t mc_meas_value_ccc_h;

} mc_service_t;


/* -------------------------------------------------------------------------
 * Char 1: relay control write/read helpers
 * ------------------------------------------------------------------------- */

static att_error_t do_char_value_write(mc_service_t *mcs, uint16_t conn_idx,
                           uint16_t offset, uint16_t length, const uint8_t *value)
{
        uint8_t ct;

        if (offset) {
                return ATT_ERROR_ATTRIBUTE_NOT_LONG;
        }
        if (length != 1) {
                return ATT_ERROR_INVALID_VALUE_LENGTH;
        }
        if (!mcs->cb || !mcs->cb->set_characteristic_value) {
                return ATT_ERROR_WRITE_NOT_PERMITTED;
        }

        ct = get_u8(value);
        mcs->cb->set_characteristic_value(&mcs->svc, conn_idx, &ct);

        return (att_error_t) -1;  /* confirmed inside the callback via mcs_set_char_value_cfm */
}

static att_error_t do_char_value_ccc_write(mc_service_t *mcs, uint16_t conn_idx,
                              uint16_t offset, uint16_t length, const uint8_t *value)
{
        uint16_t ccc;

        if (offset) {
                return ATT_ERROR_ATTRIBUTE_NOT_LONG;
        }
        if (length != sizeof(ccc)) {
                return ATT_ERROR_INVALID_VALUE_LENGTH;
        }

        ccc = get_u16(value);
        ble_storage_put_u32(conn_idx, mcs->mc_char_value_ccc_h, ccc, true);

        return ATT_ERROR_OK;
}

static void do_char_value_read(mc_service_t *mcs, const ble_evt_gatts_read_req_t *evt)
{
        if (!mcs->cb || !mcs->cb->get_characteristic_value) {
                ble_gatts_read_cfm(evt->conn_idx, evt->handle,
                                   ATT_ERROR_READ_NOT_PERMITTED, 0, NULL);
                return;
        }
        mcs->cb->get_characteristic_value(&mcs->svc, evt->conn_idx);
}

/* -------------------------------------------------------------------------
 * Char 2: measurement CCC write helper
 * ------------------------------------------------------------------------- */

static att_error_t do_meas_ccc_write(mc_service_t *mcs, uint16_t conn_idx,
                          uint16_t offset, uint16_t length, const uint8_t *value)
{
        uint16_t ccc;

        if (offset) {
                return ATT_ERROR_ATTRIBUTE_NOT_LONG;
        }
        if (length != sizeof(ccc)) {
                return ATT_ERROR_INVALID_VALUE_LENGTH;
        }

        ccc = get_u16(value);
        ble_storage_put_u32(conn_idx, mcs->mc_meas_value_ccc_h, ccc, true);
        //printf("DBG: meas CCC written 0x%04x (conn %d)\r\n", ccc, conn_idx);

        return ATT_ERROR_OK;
}

/* -------------------------------------------------------------------------
 * ATT event handlers
 * ------------------------------------------------------------------------- */

static void handle_read_req(ble_service_t *svc, const ble_evt_gatts_read_req_t *evt)
{
        mc_service_t *mcs = (mc_service_t *) svc;

        if (evt->handle == mcs->mc_char_value_h) {
                do_char_value_read(mcs, evt);

        } else if (evt->handle == mcs->mc_char_value_ccc_h) {
                uint16_t ccc = 0x0000;
                ble_storage_get_u16(evt->conn_idx, mcs->mc_char_value_ccc_h, &ccc);
                ble_gatts_read_cfm(evt->conn_idx, evt->handle, ATT_ERROR_OK,
                                   sizeof(ccc), &ccc);

        } else if (evt->handle == mcs->mc_meas_value_ccc_h) {
                uint16_t ccc = 0x0000;
                ble_storage_get_u16(evt->conn_idx, mcs->mc_meas_value_ccc_h, &ccc);
                ble_gatts_read_cfm(evt->conn_idx, evt->handle, ATT_ERROR_OK,
                                   sizeof(ccc), &ccc);

        } else {
                ble_gatts_read_cfm(evt->conn_idx, evt->handle,
                                   ATT_ERROR_READ_NOT_PERMITTED, 0, NULL);
        }
}

static void handle_write_req(ble_service_t *svc, const ble_evt_gatts_write_req_t *evt)
{
        mc_service_t *mcs = (mc_service_t *) svc;
        att_error_t status = ATT_ERROR_WRITE_NOT_PERMITTED;

        if (evt->handle == mcs->mc_char_value_h) {
                status = do_char_value_write(mcs, evt->conn_idx, evt->offset,
                                             evt->length, evt->value);

        } else if (evt->handle == mcs->mc_char_value_ccc_h) {
                status = do_char_value_ccc_write(mcs, evt->conn_idx, evt->offset,
                                                 evt->length, evt->value);

        } else if (evt->handle == mcs->mc_meas_value_ccc_h) {
                status = do_meas_ccc_write(mcs, evt->conn_idx, evt->offset,
                                           evt->length, evt->value);
        }

        if (status == ((att_error_t) - 1)) {
                /* Write handler executed properly, will be replied by cfm call */
                return;
        }

        ble_gatts_write_cfm(evt->conn_idx, evt->handle, status);
}

static void cleanup(ble_service_t *svc)
{
        mc_service_t *mcs = (mc_service_t *) svc;

        ble_storage_remove_all(mcs->mc_char_value_ccc_h);
        ble_storage_remove_all(mcs->mc_meas_value_ccc_h);

        OS_FREE(mcs);
}

/* -------------------------------------------------------------------------
 * Char 1: relay control notifications
 * ------------------------------------------------------------------------- */

void mcs_notify_char_value_all(ble_service_t *svc, const uint8_t *value)
{
        uint8_t num_conn;
        uint16_t *conn_idx;

        ble_gap_get_connected(&num_conn, &conn_idx);

        while (num_conn--) {
                mcs_notify_char_value(svc, conn_idx[num_conn], value);
        }

        if (conn_idx) {
                OS_FREE(conn_idx);
        }
}

static void mcs_notify_char_value(ble_service_t *svc, uint16_t conn_idx, const uint8_t *value)
{
        mc_service_t *mcs = (mc_service_t *) svc;
        uint16_t ccc = 0x0000;
        uint8_t pdu[1];

        ble_storage_get_u16(conn_idx, mcs->mc_char_value_ccc_h, &ccc);

        if (!(ccc & GATT_CCC_NOTIFICATIONS)) {
                return;
        }

        pdu[0] = *((uint8_t *)value);
        ble_gatts_send_event(conn_idx, mcs->mc_char_value_h, GATT_EVENT_NOTIFICATION,
                             sizeof(pdu), pdu);
}

/* -------------------------------------------------------------------------
 * Char 2: measurement notifications
 * ------------------------------------------------------------------------- */

void mcs_notify_meas_all(ble_service_t *svc, const meas_packet_t *pkt)
{
        uint8_t num_conn;
        uint16_t *conn_idx;

        ble_gap_get_connected(&num_conn, &conn_idx);

        while (num_conn--) {
                mcs_notify_meas(svc, conn_idx[num_conn], pkt);
        }

        if (conn_idx) {
                OS_FREE(conn_idx);
        }
}

static void mcs_notify_meas(ble_service_t *svc, uint16_t conn_idx, const meas_packet_t *pkt)
{
        mc_service_t *mcs = (mc_service_t *) svc;
        uint16_t ccc = 0x0000;

        ble_storage_get_u16(conn_idx, mcs->mc_meas_value_ccc_h, &ccc);
        //printf("DBG: notify_meas conn=%d ccc=0x%04x\r\n", conn_idx, ccc);

        if (!(ccc & GATT_CCC_NOTIFICATIONS)) {
                return;
        }

        ble_gatts_send_event(conn_idx, mcs->mc_meas_value_h, GATT_EVENT_NOTIFICATION,
                             sizeof(meas_packet_t), (const uint8_t *)pkt);
}

/* -------------------------------------------------------------------------
 * Public API: confirm functions (char 1 only)
 * ------------------------------------------------------------------------- */

void mcs_get_char_value_cfm(ble_service_t *svc, uint16_t conn_idx, att_error_t status,
                                                                    const uint8_t *value)
{
        mc_service_t *mcs = (mc_service_t *) svc;
        uint8_t pdu[1];

        pdu[0] = *value;
        ble_gatts_read_cfm(conn_idx, mcs->mc_char_value_h, ATT_ERROR_OK,
                           sizeof(pdu), pdu);
}

void mcs_set_char_value_cfm(ble_service_t *svc, uint16_t conn_idx, att_error_t status)
{
        mc_service_t *mcs = (mc_service_t *) svc;
        ble_gatts_write_cfm(conn_idx, mcs->mc_char_value_h, status);
}

/* -------------------------------------------------------------------------
 * Service initialisation
 * ------------------------------------------------------------------------- */

ble_service_t *mcs_init(const uint8_t *variable_value, const my_custom_service_cb_t *cb)
{
        mc_service_t *mcs;
        uint16_t num_attr;
        att_uuid_t uuid;
        uint16_t char_user_descriptor_h;

        mcs = (mc_service_t *)OS_MALLOC(sizeof(*mcs));
        memset(mcs, 0, sizeof(*mcs));

        mcs->svc.read_req  = handle_read_req;
        mcs->svc.write_req = handle_write_req;
        mcs->svc.cleanup   = cleanup;
        mcs->cb = cb;

        /*
         * 0 included services
         * 2 characteristic declarations  (relay control + measurements)
         * 3 descriptors                  (relay CCC + relay CUD + meas CCC)
         *
         * ble_gatts_get_num_attr(0, 2, 3) = 1 + 0 + 2*2 + 3 = 8 attributes
         */
        num_attr = ble_gatts_get_num_attr(0, 2, 3);

        /* Service declaration */
        ble_uuid_from_string("00000000-1111-2222-2222-333333333333", &uuid);
        ble_gatts_add_service(&uuid, GATT_SERVICE_PRIMARY, num_attr);

        /* --- Char 1: relay control (R/W/Notify, 1 byte) --- */
        ble_uuid_from_string("11111111-0000-0000-0000-111111111111", &uuid);
        ble_gatts_add_characteristic(&uuid,
                GATT_PROP_READ | GATT_PROP_NOTIFY | GATT_PROP_WRITE,
                ATT_PERM_RW, 1, GATTS_FLAG_CHAR_READ_REQ,
                NULL, &mcs->mc_char_value_h);

        /* CCC descriptor for relay control */
        ble_uuid_create16(UUID_GATT_CLIENT_CHAR_CONFIGURATION, &uuid);
        ble_gatts_add_descriptor(&uuid, ATT_PERM_RW, 2, 0, &mcs->mc_char_value_ccc_h);

        /* CUD descriptor for relay control */
        ble_uuid_create16(UUID_GATT_CHAR_USER_DESCRIPTION, &uuid);
        ble_gatts_add_descriptor(&uuid, ATT_PERM_READ, sizeof(char_user_descriptor_val),
                                 0, &char_user_descriptor_h);

        /* --- Char 2: measurements (Notify only, 15 bytes) --- */
        ble_uuid_from_string("22222222-0000-0000-0000-222222222222", &uuid);
        ble_gatts_add_characteristic(&uuid,
                GATT_PROP_NOTIFY,
                ATT_PERM_NONE, sizeof(meas_packet_t), 0,
                NULL, &mcs->mc_meas_value_h);

        /* CCC descriptor for measurements */
        ble_uuid_create16(UUID_GATT_CLIENT_CHAR_CONFIGURATION, &uuid);
        ble_gatts_add_descriptor(&uuid, ATT_PERM_RW, 2, 0, &mcs->mc_meas_value_ccc_h);

        /* Register all handles so the BLE manager can auto-update them */
        ble_gatts_register_service(&mcs->svc.start_h,
                &mcs->mc_char_value_h,
                &mcs->mc_char_value_ccc_h,
                &char_user_descriptor_h,
                &mcs->mc_meas_value_h,
                &mcs->mc_meas_value_ccc_h,
                0);

        mcs->svc.end_h = mcs->svc.start_h + num_attr;

        /* Set default values */
        ble_gatts_set_value(mcs->mc_char_value_h, 1, variable_value);
        ble_gatts_set_value(char_user_descriptor_h, sizeof(char_user_descriptor_val),
                            char_user_descriptor_val);

        ble_service_add(&mcs->svc);

        return &mcs->svc;
}
