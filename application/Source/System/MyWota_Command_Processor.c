/*
 * @attention
 * Copyright (c) Sevantica 2026.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 * ******************************************************************************
 */

/**
 * @file MyWota_Command_Processor.c
 * @brief Unified Command Processor & Dispatcher for USB & RS485 origins
 */

/* Includes ------------------------------------------------------------------*/
#include "MyWota_Command_Processor.h"
#include "USB_Logging.h"
#include "RS485_Protocol.h"
#include "RS485_Slave.h"
#include "Dispenser_Controller.h"
#include "Fault_Manager.h"
#include "pico/time.h"
#include "hardware/watchdog.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdlib.h>

/* Bind the real, channel-less USB logger before USB_Log_Printf is re-aliased
 * below to the CLI-channel variant. RS485-origin handlers have no CLI channel,
 * so their diagnostics log through this pointer instead. */
static int (* const rs485_diag_log)(const char *format, ...) = USB_Log_Printf;

#ifdef USB_Log_Printf
#undef USB_Log_Printf
#endif
#define USB_Log_Printf(...) CLI_Printf(channel, __VA_ARGS__)

/* Private Function Prototypes -----------------------------------------------*/
static void dispatch_usb(const System_Command_Request_t *request, System_Command_Status_t status, const CLI_Channel_t* channel);
static void dispatch_rs485(const System_Command_Request_t *request, System_Command_Status_t status);
static void send_nak(uint8_t sequence, RS485_NAK_Reason_t reason);
static CLI_Command_Status_t usb_status_from_system(System_Command_Status_t status);

/* Public Functions ----------------------------------------------------------*/

System_Command_Status_t MyWota_Command_Processor_Execute(const System_Command_Request_t *request, System_Command_Response_t *response, const CLI_Channel_t* channel)
{
    if (request == NULL) {
        return SYSTEM_CMD_STATUS_INVALID_PARAM;
    }

    System_Command_Response_t local_resp = {0};
    if (response == NULL) {
        response = &local_resp;
    }

    /* 1. Execute using the central System Command executor */
    System_Command_Status_t status = System_Command_Execute(request, response);

    /* 2. Unified response dispatching based on request origin */
    if (request->origin == SYSTEM_CMD_ORIGIN_LOCAL_USB) {
        dispatch_usb(request, status, channel);
    } else if (request->origin == SYSTEM_CMD_ORIGIN_RS485) {
        dispatch_rs485(request, status);
    }

    return status;
}

CLI_Command_Status_t MyWota_Command_Processor_ExecuteUSB(const CLI_Channel_t* channel, int argc, char** argv)
{
    if (argc < 1 || argv[0] == NULL) {
        return CLI_CMD_ERROR;
    }

    System_Command_Request_t request = {
        .origin = SYSTEM_CMD_ORIGIN_LOCAL_USB,
    };

    if (strcmp(argv[0], "dispenser") == 0) {
        if (argc < 2) {
            USB_Log_Printf("Usage: dispenser <command>\r\n");
            USB_Log_Printf("Commands:\r\n");
            USB_Log_Printf("  start [L]    - Start manual dispense (min 1L)\r\n");
            USB_Log_Printf("  stop         - Stop manual dispense\r\n");
            USB_Log_Printf("  topup <L>    - Topup card balance (min 1L)\r\n");
            return CLI_CMD_ERROR_INVALID_PARAM;
        }

        const char* subcmd = argv[1];
        if (strcmp(subcmd, "start") == 0) {
            uint32_t volume_l = 0;
            uint32_t volume_ml = 0;
            if (argc >= 3) {
                volume_l = (uint32_t)atoi(argv[2]);
                if (volume_l < 1) {
                    USB_Log_Printf("[✗] Invalid volume (min 1L)\r\n");
                    return CLI_CMD_ERROR_INVALID_PARAM;
                }
                if (volume_l > 1000) {
                    USB_Log_Printf("[✗] Volume too large (max 1000 L)\r\n");
                    return CLI_CMD_ERROR_INVALID_PARAM;
                }
                volume_ml = volume_l * 1000;
            }
            request.id = SYSTEM_CMD_ID_DISPENSE_START;
            request.param1 = volume_ml;
        }
        else if (strcmp(subcmd, "stop") == 0) {
            request.id = SYSTEM_CMD_ID_DISPENSE_STOP;
        }
        else if (strcmp(subcmd, "topup") == 0) {
            if (argc < 3) {
                USB_Log_Printf("[✗] Usage: dispenser topup <amount_L>\r\n");
                return CLI_CMD_ERROR_INVALID_PARAM;
            }
            uint32_t amount_l = (uint32_t)atoi(argv[2]);
            if (amount_l < 1) {
                USB_Log_Printf("[✗] Invalid amount (min 1L)\r\n");
                return CLI_CMD_ERROR_INVALID_PARAM;
            }
            request.id = SYSTEM_CMD_ID_CARD_TOPUP;
            request.param1 = amount_l * 1000;
        }
        else {
            USB_Log_Printf("[✗] Unknown dispenser command: %s\r\n", subcmd);
            return CLI_CMD_ERROR_UNKNOWN_COMMAND;
        }
    }
    else if (strcmp(argv[0], "clean") == 0) {
        if (argc < 2) {
            USB_Log_Printf("Usage: clean <start [vol_ml] [max_sec] | stop | status>\r\n");
            return CLI_CMD_ERROR_INVALID_PARAM;
        }

        const char* sub = argv[1];
        if (strcmp(sub, "start") == 0) {
            uint32_t vol = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 0;
            uint32_t sec = (argc >= 4) ? (uint32_t)atoi(argv[3]) : 0;
            request.id = SYSTEM_CMD_ID_CLEAN_START;
            request.param1 = vol;
            request.param2 = sec;
        }
        else if (strcmp(sub, "stop") == 0) {
            request.id = SYSTEM_CMD_ID_CLEAN_STOP;
        }
        else if (strcmp(sub, "status") == 0) {
            USB_Log_Printf("Self-clean active: %s\r\n", Dispenser_IsSelfCleaning() ? "YES" : "no");
            USB_Log_Printf("Last clean (unix): %lu\r\n", (unsigned long)Dispenser_GetLastCleanUnixTime());
            return CLI_CMD_OK;
        }
        else {
            USB_Log_Printf("[✗] Unknown clean subcommand: %s\r\n", sub);
            return CLI_CMD_ERROR_UNKNOWN_COMMAND;
        }
    }
    else if (strcmp(argv[0], "fault") == 0) {
        if (argc < 2) {
            USB_Log_Printf("Usage: fault <status|clear>\r\n");
            return CLI_CMD_ERROR_INVALID_PARAM;
        }

        const char* sub = argv[1];
        if (strcmp(sub, "status") == 0) {
            USB_Log_Printf("Fault state:       %s\r\n", Fault_Manager_GetStateString(Fault_Manager_GetState()));
            USB_Log_Printf("Fault reason:      %s\r\n", Fault_Manager_GetReasonString(Fault_Manager_GetReason()));
            USB_Log_Printf("Incidents (boot):  %lu\r\n", (unsigned long)Fault_Manager_GetIncidentCount());
            return CLI_CMD_OK;
        }
        else if (strcmp(sub, "clear") == 0) {
            request.id = SYSTEM_CMD_ID_FAULT_CLEAR;
        }
        else {
            USB_Log_Printf("[✗] Unknown fault subcommand: %s\r\n", sub);
            return CLI_CMD_ERROR_UNKNOWN_COMMAND;
        }
    }
    else {
        return CLI_CMD_ERROR_UNKNOWN_COMMAND;
    }

    System_Command_Response_t response = {0};
    System_Command_Status_t status = MyWota_Command_Processor_Execute(&request, &response, channel);

    return usb_status_from_system(status);
}

bool MyWota_Command_Processor_ExecuteRS485(const RS485_Frame_t *rx_frame, uint8_t sequence)
{
    if (rx_frame == NULL) {
        return false;
    }

    System_Command_Request_t request = {
        .origin = SYSTEM_CMD_ORIGIN_RS485,
        .session_id = sequence,
    };

    switch (rx_frame->header.command) {
        case RS485_CMD_RESET:
            request.id = SYSTEM_CMD_ID_RESET;
            MyWota_Command_Processor_Execute(&request, NULL, NULL);
            // Re-arm watchdog / trigger RP2040 reboot
            busy_wait_us(20000);
            watchdog_reboot(0, 0, 0);
            while (1) { }
            return true;

        case RS485_CMD_SYNC_TIME:
            if (rx_frame->header.length < sizeof(RS485_SyncTime_Payload_t)) {
                send_nak(sequence, RS485_NAK_INVALID_PARAM);
                return true;
            }
            {
                const RS485_SyncTime_Payload_t* p = (const RS485_SyncTime_Payload_t*)rx_frame->payload;
                request.id = SYSTEM_CMD_ID_RTC_SYNC;
                request.param1 = p->unix_time;
            }
            break;

        case RS485_CMD_TRIGGER_CLEAN:
            if (rx_frame->header.length < sizeof(RS485_TriggerClean_Payload_t)) {
                send_nak(sequence, RS485_NAK_INVALID_PARAM);
                return true;
            }
            {
                const RS485_TriggerClean_Payload_t* p = (const RS485_TriggerClean_Payload_t*)rx_frame->payload;
                if (p->target_volume_ml == 0) {
                    request.id = SYSTEM_CMD_ID_CLEAN_STOP;
                } else {
                    request.id = SYSTEM_CMD_ID_CLEAN_START;
                    request.param1 = p->target_volume_ml;
                    request.param2 = p->max_duration_sec;
                }
            }
            break;

        case RS485_CMD_DISPENSE_START:
            {
                uint32_t volume_ml = 0;
                if (rx_frame->header.length >= sizeof(RS485_DispenseStart_Payload_t)) {
                    const RS485_DispenseStart_Payload_t* p = (const RS485_DispenseStart_Payload_t*)rx_frame->payload;
                    volume_ml = p->target_volume_ml;
                }
                request.id = SYSTEM_CMD_ID_DISPENSE_START;
                request.param1 = volume_ml;
            }
            break;

        case RS485_CMD_DISPENSE_STOP:
            request.id = SYSTEM_CMD_ID_DISPENSE_STOP;
            break;

        case RS485_CMD_CARD_UPDATE:
            if (rx_frame->header.length < sizeof(RS485_CardUpdate_Payload_t)) {
                send_nak(sequence, RS485_NAK_INVALID_PARAM);
                return true;
            }
            {
                const RS485_CardUpdate_Payload_t* p = (const RS485_CardUpdate_Payload_t*)rx_frame->payload;
                request.id = SYSTEM_CMD_ID_CARD_TOPUP;
                request.param1 = p->topup_amount;
            }
            break;

        case RS485_CMD_FAULT_CLEAR:
            request.id = SYSTEM_CMD_ID_FAULT_CLEAR;
            break;

        case RS485_CMD_ADMIN_AUTH_GRANT:
            if (rx_frame->header.length < sizeof(RS485_AdminAuthGrant_Payload_t)) {
                send_nak(sequence, RS485_NAK_INVALID_PARAM);
                return true;
            }
            {
                RS485_AdminAuthGrant_Payload_t payload;
                memcpy(&payload, rx_frame->payload, sizeof(payload));
                request.id = SYSTEM_CMD_ID_ADMIN_GRANT;
                request.param1 = payload.session_id;
                request.param2 = payload.duration_ms;
                rs485_diag_log("[RS485 Slave] RX ADMIN_AUTH_GRANT seq=%u session=0x%08lX duration=%lums\r\n",
                               (unsigned)sequence,
                               (unsigned long)payload.session_id,
                               (unsigned long)payload.duration_ms);
            }
            break;

        case RS485_CMD_ADMIN_AUTH_CLEAR:
            request.id = SYSTEM_CMD_ID_ADMIN_CLEAR;
            break;

        default:
            return false; /* Not a central processor control command */
    }

    MyWota_Command_Processor_Execute(&request, NULL, NULL);

    /* Post-execution cleanup: a single-command remote admin grant (session
     * 0xFFFFFFFF) must persist until the *next* command consumes it. Clearing
     * it here only makes sense for non-grant commands; doing it on the grant
     * command itself would revoke admin before the privileged command (e.g.
     * TRIGGER_CLEAN) ever arrives, causing it to be rejected as unauthorized. */
    if (rx_frame->header.command != RS485_CMD_ADMIN_AUTH_GRANT &&
        System_Command_GetRemoteAdminSessionID() == 0xFFFFFFFFu) {
        System_Command_ClearRemoteAdminGrant();
    }

    return true;
}

/* Private Functions ---------------------------------------------------------*/

static void dispatch_usb(const System_Command_Request_t *request, System_Command_Status_t status, const CLI_Channel_t* channel)
{
    switch (request->id) {
        case SYSTEM_CMD_ID_DISPENSE_START:
            if (status == SYSTEM_CMD_STATUS_OK) {
                if (request->param1 == 0) {
                    USB_Log_Printf("[✓] Manual dispense started (unlimited)\r\n");
                } else {
                    USB_Log_Printf("[✓] Manual dispense started for %lu L (%lu ml)\r\n", request->param1 / 1000u, request->param1);
                }
            } else {
                USB_Log_Printf("[✗] Failed to start dispense: %s\r\n", System_Command_GetStatusString(status));
            }
            break;

        case SYSTEM_CMD_ID_DISPENSE_STOP:
            if (status == SYSTEM_CMD_STATUS_OK) {
                USB_Log_Printf("[✓] Dispense stopped\r\n");
            } else {
                USB_Log_Printf("[✗] Failed to stop dispense: %s\r\n", System_Command_GetStatusString(status));
            }
            break;

        case SYSTEM_CMD_ID_CARD_TOPUP:
            if (status == SYSTEM_CMD_STATUS_OK) {
                USB_Log_Printf("[✓] Card topup completed successfully\r\n");
            } else {
                USB_Log_Printf("[✗] Card topup failed: %s\r\n", System_Command_GetStatusString(status));
            }
            break;

        case SYSTEM_CMD_ID_CLEAN_START:
            if (status == SYSTEM_CMD_STATUS_OK) {
                USB_Log_Printf("[→] Self-clean started\r\n");
            } else {
                USB_Log_Printf("[✗] Self-clean refused: %s\r\n", System_Command_GetStatusString(status));
            }
            break;

        case SYSTEM_CMD_ID_CLEAN_STOP:
            if (status == SYSTEM_CMD_STATUS_OK) {
                USB_Log_Printf("[→] Self-clean stop requested\r\n");
            } else {
                USB_Log_Printf("[✗] Self-clean stop refused: %s\r\n", System_Command_GetStatusString(status));
            }
            break;

        case SYSTEM_CMD_ID_FAULT_CLEAR:
            if (status == SYSTEM_CMD_STATUS_OK) {
                USB_Log_Printf("[✓] Fault cleared\r\n");
            } else {
                USB_Log_Printf("[✗] Fault clear refused: %s\r\n", System_Command_GetStatusString(status));
            }
            break;

        case SYSTEM_CMD_ID_RTC_SYNC:
            if (status == SYSTEM_CMD_STATUS_OK) {
                USB_Log_Printf("[✓] RTC synced to %lu\r\n", (unsigned long)request->param1);
            } else {
                USB_Log_Printf("[✗] RTC sync failed: %s\r\n", System_Command_GetStatusString(status));
            }
            break;

        case SYSTEM_CMD_ID_RESET:
            if (status == SYSTEM_CMD_STATUS_OK) {
                USB_Log_Printf("[✓] System reset requested\r\n");
            } else {
                USB_Log_Printf("[✗] System reset refused: %s\r\n", System_Command_GetStatusString(status));
            }
            break;

        case SYSTEM_CMD_ID_ADMIN_GRANT:
            if (status == SYSTEM_CMD_STATUS_OK) {
                USB_Log_Printf("[✓] Remote Admin Grant session=%lu duration=%lu ms\r\n", request->param1, request->param2);
            } else {
                USB_Log_Printf("[✗] Remote Admin Grant refused: %s\r\n", System_Command_GetStatusString(status));
            }
            break;

        case SYSTEM_CMD_ID_ADMIN_CLEAR:
            if (status == SYSTEM_CMD_STATUS_OK) {
                USB_Log_Printf("[✓] Remote Admin Grant cleared\r\n");
            } else {
                USB_Log_Printf("[✗] Remote Admin Clear refused: %s\r\n", System_Command_GetStatusString(status));
            }
            break;

        default:
            if (status == SYSTEM_CMD_STATUS_OK) {
                USB_Log_Printf("[✓] Command %d executed successfully\r\n", request->id);
            } else {
                USB_Log_Printf("[✗] Command %d failed: %s\r\n", request->id, System_Command_GetStatusString(status));
            }
            break;
    }
}

static void dispatch_rs485(const System_Command_Request_t *request, System_Command_Status_t status)
{
    uint8_t sequence = (uint8_t)request->session_id;

    if (request->id == SYSTEM_CMD_ID_ADMIN_GRANT) {
        rs485_diag_log("[RS485 Slave] TX %s for ADMIN_AUTH_GRANT seq=%u (status=%d)\r\n",
                       (status == SYSTEM_CMD_STATUS_OK) ? "ACK" : "NAK",
                       (unsigned)sequence, (int)status);
    }

    if (status == SYSTEM_CMD_STATUS_OK) {
        RS485_Frame_t* response_frame = RS485_GetScratchFrame();
        RS485_BuildFrame(response_frame, RS485_ADDR_MASTER, RS485_CMD_ACK, sequence, NULL, 0);
        RS485_SendFrame(response_frame);
    } else {
        RS485_NAK_Reason_t reason = RS485_NAK_BUSY;
        if (status == SYSTEM_CMD_STATUS_UNAUTHORIZED) {
            reason = RS485_NAK_NOT_AUTHORIZED;
        } else if (status == SYSTEM_CMD_STATUS_INVALID_PARAM) {
            reason = RS485_NAK_INVALID_PARAM;
        } else if (status == SYSTEM_CMD_STATUS_NOT_READY) {
            reason = RS485_NAK_NOT_READY;
        }

        RS485_Frame_t* response_frame = RS485_GetScratchFrame();
        uint8_t payload = (uint8_t)reason;
        RS485_BuildFrame(response_frame, RS485_ADDR_MASTER, RS485_CMD_NAK, sequence, &payload, 1);
        RS485_SendFrame(response_frame);
    }
}

static CLI_Command_Status_t usb_status_from_system(System_Command_Status_t status)
{
    switch (status) {
        case SYSTEM_CMD_STATUS_OK:
            return CLI_CMD_OK;
        case SYSTEM_CMD_STATUS_UNKNOWN_COMMAND:
            return CLI_CMD_ERROR_UNKNOWN_COMMAND;
        case SYSTEM_CMD_STATUS_INVALID_PARAM:
            return CLI_CMD_ERROR_INVALID_PARAM;
        default:
            return CLI_CMD_ERROR;
    }
}

static void send_nak(uint8_t sequence, RS485_NAK_Reason_t reason)
{
    RS485_Frame_t* response_frame = RS485_GetScratchFrame();
    uint8_t payload = (uint8_t)reason;
    RS485_BuildFrame(response_frame, RS485_ADDR_MASTER, RS485_CMD_NAK, sequence, &payload, 1);
    RS485_SendFrame(response_frame);
}
