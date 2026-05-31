/*
 * @attention
 * Copyright (c) Sevantica 2026.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

/**
 * @file RS485_Command_Adapter.c
 * @brief MyWota RS485 Command Handlers
 * @details Implements project-specific RS485 commands for water dispenser control
 */

/* Includes ------------------------------------------------------------------*/
#include "RS485_Command_Adapter.h"
#include "MyWota_Command_Processor.h"
#include "RS485_Task.h"
#include "RS485_Protocol.h"
#include "RS485_Slave_Common_Handlers.h"
#include "System_Command.h"
#include "Dispenser_Controller.h"
#include "Fault_Manager.h"
#include "MIFARE_Transaction_Core.h"
#include "PN532_Driver.h"
#include "Application_Interface.h"
#include "RTC_Manager.h"
#include "RTC_Task.h"
#include "SD_Logger_Task.h"
#include "Feedback_Task.h"
#include "mywota_ui_driver.h"
#include "MyWota_System.h"
#include "USB_Logging.h"
#include "USB_Command_Handler.h"
#include "CLI_Processor.h"
#include "Firmware_Version.h"
#include "pico/unique_id.h"
#include "pico/time.h"
#include "hardware/watchdog.h"
#include <string.h>

/* Hardware Revision --------------------------------------------------------*/
#ifndef RS485_ADAPTER_HW_REVISION
#define RS485_ADAPTER_HW_REVISION  1u
#endif

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_RS485_ADAPTER_EN  1

#if LOG_DEBUG_RS485_ADAPTER_EN
    #define LOG_DEBUG_ADAPTER(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_ADAPTER(...)
#endif

/* Private Function Prototypes -----------------------------------------------*/
static bool cmd_poll_status(const RS485_Frame_t *rx_frame, uint8_t sequence);
static bool cmd_get_device_info(const RS485_Frame_t *rx_frame, uint8_t sequence);
static bool cmd_get_config(const RS485_Frame_t *rx_frame, uint8_t sequence);
static bool cmd_heartbeat(const RS485_Frame_t *rx_frame, uint8_t sequence);
static bool cmd_control_forward(const RS485_Frame_t *rx_frame, uint8_t sequence);
static bool cmd_debug_cmd(const RS485_Frame_t *rx_frame, uint8_t sequence);
static bool cmd_debug_fetch(const RS485_Frame_t *rx_frame, uint8_t sequence);

/* Helper Functions */
static void rs485_send_ack(uint8_t sequence);
static void rs485_send_nak(uint8_t sequence, RS485_NAK_Reason_t reason);
static void rs485_send_response(uint8_t sequence, RS485_Command_t command, 
                                 const void* payload, uint16_t length);
static void rs485_fw_exclusive_changed(bool active, void *context);
static void rs485_set_optional_task(TaskHandle_t handle, System_Task_ID_t task_id, bool suspend);

/* Command Table -------------------------------------------------------------*/
static const RS485_Command_Entry_t adapter_commands[] = {
    {RS485_CMD_POLL_STATUS,    cmd_poll_status,      "Poll dispenser status"},
    {RS485_CMD_GET_INFO,       cmd_get_device_info,  "Get device information"},
    {RS485_CMD_GET_CONFIG,     cmd_get_config,       "Get configuration"},
    {RS485_CMD_RESET,          cmd_control_forward,  "Reset device"},
    {RS485_CMD_HEARTBEAT,      cmd_heartbeat,        "Heartbeat ack"},
    {RS485_CMD_SYNC_TIME,      cmd_control_forward,  "Sync RTC"},
    {RS485_CMD_TRIGGER_CLEAN,  cmd_control_forward,  "Self-clean cycle"},
    {RS485_CMD_DISPENSE_START, cmd_control_forward,  "Manual dispense start"},
    {RS485_CMD_DISPENSE_STOP,  cmd_control_forward,  "Stop dispense"},
    {RS485_CMD_CARD_UPDATE,    cmd_control_forward,  "Card top-up"},
    {RS485_CMD_FAULT_CLEAR,    cmd_control_forward,  "Clear latched fault"},
    {RS485_CMD_ADMIN_AUTH_GRANT, cmd_control_forward, "Grant admin authorization"},
    {RS485_CMD_ADMIN_AUTH_CLEAR, cmd_control_forward, "Clear admin authorization"},
    /* Shared firmware-update + transaction-sync handlers */
    {RS485_CMD_FW_START,         RS485_SlaveCommon_FW_HandleStart,   "FW update start"},
    {RS485_CMD_FW_DATA,          RS485_SlaveCommon_FW_HandleData,    "FW data chunk"},
    {RS485_CMD_FW_VERIFY,        RS485_SlaveCommon_FW_HandleVerify,  "FW verify image"},
    {RS485_CMD_FW_APPLY,         RS485_SlaveCommon_FW_HandleApply,   "FW apply / reboot"},
    {RS485_CMD_GET_TRANSACTIONS, RS485_SlaveCommon_TxnHandleGet,     "Pull pending transactions"},
    {RS485_CMD_TRANSACTION_ACK,  RS485_SlaveCommon_TxnHandleAck,     "Ack uploaded transactions"},
    /* Debug & Remote command output capture handlers */
    {RS485_CMD_DEBUG_CMD,        cmd_debug_cmd,                      "Execute remote debug command"},
    {RS485_CMD_DEBUG_FETCH,      cmd_debug_fetch,                    "Fetch debug output chunk"},
};

static const RS485_Command_Interface_t command_interface = {
    .commands = adapter_commands,
    .command_count = sizeof(adapter_commands) / sizeof(adapter_commands[0]),
    .project_name = "MyWota Water Dispenser"
};

/* Public Functions ----------------------------------------------------------*/

/**
 * @brief Initialize RS485 command adapter
 */
RS485_Result_t RS485_Command_Adapter_Init(void)
{
    LOG_DEBUG_ADAPTER("[RS485_ADAPTER] Registering %zu MyWota commands\r\n", 
                      command_interface.command_count);
    
    RS485_Result_t result = RS485_RegisterCommandInterface(&command_interface);
    
    if (result == RS485_OK) {
        LOG_DEBUG_ADAPTER("[RS485_ADAPTER] Commands registered successfully\r\n");
    } else {
        LOG_DEBUG_ADAPTER("[RS485_ADAPTER] Failed to register commands\r\n");
    }

    /* Stage firmware updates in upper half of flash. PICO_FLASH_SIZE_BYTES is
     * supplied by the pico-sdk; fall back to 2 MB if undefined. */
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2u * 1024u * 1024u)
#endif
    const uint32_t fw_bootloader_size = 64u * 1024u;
    const uint32_t fw_app_max_size = 980u * 1024u;
    const uint32_t fw_stage_offset = fw_bootloader_size + fw_app_max_size;
    const uint32_t fw_stage_max = (uint32_t)PICO_FLASH_SIZE_BYTES - fw_stage_offset - (16u * 1024u);
    RS485_SlaveCommon_FW_Init(fw_stage_offset, fw_stage_max);
    RS485_SlaveCommon_FW_SetExclusiveCallback(rs485_fw_exclusive_changed, NULL);
    
    return result;
}

static void rs485_set_optional_task(TaskHandle_t handle, System_Task_ID_t task_id, bool suspend)
{
    if (handle == NULL) {
        return;
    }

    if (suspend) {
        System_SetTaskMonitoringEnabled(task_id, false);
        vTaskSuspend(handle);
    } else {
        vTaskResume(handle);
        System_SetTaskMonitoringEnabled(task_id, true);
        System_ReportTaskStatus(task_id, true);
    }
}

static void rs485_fw_exclusive_changed(bool active, void *context)
{
    (void)context;

    if (active) {
        LOG_DEBUG_ADAPTER("[RS485_ADAPTER] Firmware update exclusive mode: pausing optional tasks\r\n");
        if (Dispenser_IsDispenseActive()) {
            (void)MIFARE_Dispenser_EmergencyStop();
        }
    } else {
        LOG_DEBUG_ADAPTER("[RS485_ADAPTER] Firmware update exclusive mode ended: resuming optional tasks\r\n");
    }

    rs485_set_optional_task(task_get_handle_MIFARE_Polling_Task(), SYSTEM_TASK_ID_MIFARE_POLLING, active);
    rs485_set_optional_task(Dispenser_Task_GetHandle(), SYSTEM_TASK_ID_DISPENSER, active);
    rs485_set_optional_task(task_get_handle_LCD_Display_Driver_Task(), SYSTEM_TASK_ID_LCD_DISPLAY, active);
    rs485_set_optional_task(Feedback_Task_GetHandle(), SYSTEM_TASK_ID_BUZZER_POLLING, active);
    rs485_set_optional_task(SD_Logger_Task_GetHandle(), SYSTEM_TASK_ID_SD_LOGGER, active);
    rs485_set_optional_task(RTC_Task_GetHandle(), SYSTEM_TASK_ID_RTC, active);
}

/* Private Functions ---------------------------------------------------------*/

/**
 * @brief Handle POLL_STATUS command
 * @details Returns current car wash status including balance, state, and card info
 */
static bool cmd_poll_status(const RS485_Frame_t *rx_frame, uint8_t sequence)
{
    (void)rx_frame;  /* No payload expected */
    
    RS485_Device_Status_t status = {0};
    
    /* Get application instance */
    const Application_Instance_t* app = Application_GetActive();
    
    if (app != NULL && app->callbacks != NULL) {
        /* Fill device type */
        status.device_type = app->type;
        
        /* Get application state */
        if (app->callbacks->get_state != NULL) {
            status.state = app->callbacks->get_state();
        }
        
        /* Get balance */
        if (app->callbacks->get_primary_balance != NULL) {
            status.balance = app->callbacks->get_primary_balance();
        }
        
        /* Check if operation active */
        if (app->callbacks->is_operation_active != NULL) {
            if (app->callbacks->is_operation_active()) {
                status.flags |= RS485_STATUS_FLAG_DISPENSING;
            }
        }
    }

    /* Pressure / booster pump request (CCH aggregates across all slaves). */
    Dispenser_GetPeripheralRequest(&status.peripheral_request_id, &status.peripheral_request_level);

    /* Flow diagnostics: latest + 8-sample min/max ring; valve commanded state.
     * Sampled here so the ring advances exactly once per master poll.
     * Use locals to avoid taking pointers to packed-struct members. */
    uint16_t flow_now = 0, flow_min = 0, flow_max = 0;
    Dispenser_SampleFlowDiagnostics(&flow_now, &flow_min, &flow_max);
    status.flow_clpm     = flow_now;
    status.flow_clpm_min = flow_min;
    status.flow_clpm_max = flow_max;
    status.valve_cmd     = Dispenser_IsValveCommanded() ? 1u : 0u;
    uint32_t admin_remaining_ms = System_Command_GetAdminRemainingMs();
    if (admin_remaining_ms > 0u) {
        status.flags |= RS485_STATUS_FLAG_ADMIN_AUTH;
        uint32_t admin_remaining_sec = (admin_remaining_ms + 999u) / 1000u;
        status.reserved_v2 = (admin_remaining_sec > 255u) ? 255u : (uint8_t)admin_remaining_sec;
    } else {
        status.reserved_v2 = 0u;
    }
    
    /* Get card information if available */
    if (MIFARE_IsCardReady()) {
        status.flags |= RS485_STATUS_FLAG_CARD_PRESENT;
        
        /* Get card info using existing API */
        PN532_CardInfo_t card_info;
        if (MIFARE_GetCurrentCardInfo(&card_info)) {
            uint8_t uid_len = (card_info.uid_length <= 7) ? card_info.uid_length : 7;
            memcpy(status.card_uid, card_info.uid, uid_len);
            status.card_uid_length = uid_len;
        }
    }
    
    /* Send status response */
    rs485_send_response(sequence, RS485_CMD_STATUS_RESPONSE, 
                        &status, sizeof(RS485_Device_Status_t));
    
    LOG_DEBUG_ADAPTER("[RS485_ADAPTER] Status sent: state=%d, balance=%lu, card=%s\r\n",
                      status.state, status.balance, 
                      (status.flags & RS485_STATUS_FLAG_CARD_PRESENT) ? "present" : "absent");
    
    return true;
}

/**
 * @brief Handle GET_INFO command
 * @details Returns device information (type, firmware version, serial number)
 */
static bool cmd_get_device_info(const RS485_Frame_t *rx_frame, uint8_t sequence)
{
    (void)rx_frame;  /* No payload expected */
    
    RS485_Device_Info_t info = {0};
    
    /* Device type */
    const Application_Instance_t* app = Application_GetActive();
    if (app != NULL) {
        info.device_type = app->type;
    }
    
    /* Firmware version */
    info.fw_version_major = FW_VERSION_MAJOR;
    info.fw_version_minor = FW_VERSION_MINOR;
    info.fw_version_patch = FW_VERSION_PATCH;
    
    /* Serial number from RP2040/RP2350 unique flash board ID (folded to 32 bits) */
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    uint32_t lo = ((uint32_t)board_id.id[0])       | ((uint32_t)board_id.id[1] << 8) |
                  ((uint32_t)board_id.id[2] << 16) | ((uint32_t)board_id.id[3] << 24);
    uint32_t hi = ((uint32_t)board_id.id[4])       | ((uint32_t)board_id.id[5] << 8) |
                  ((uint32_t)board_id.id[6] << 16) | ((uint32_t)board_id.id[7] << 24);
    info.serial_number = lo ^ hi;

    /* Hardware revision */
    info.hw_revision = RS485_ADAPTER_HW_REVISION;
    
    /* Capabilities */
    info.capabilities = 0;
    info.capabilities |= (1 << 0);  /* MIFARE support */
    info.capabilities |= (1 << 1);  /* SD logging support */
    info.capabilities |= (1 << 2);  /* RTC support */
    
    /* Send info response */
    rs485_send_response(sequence, RS485_CMD_GET_INFO, 
                        &info, sizeof(RS485_Device_Info_t));
    
    LOG_DEBUG_ADAPTER("[RS485_ADAPTER] Device info sent: FW v%d.%d.%d\r\n",
                      info.fw_version_major, info.fw_version_minor, info.fw_version_patch);
    
    return true;
}

/**
 * @brief Handle GET_CONFIG command
 * @details Returns a compact configuration / health snapshot.
 */
static bool cmd_get_config(const RS485_Frame_t *rx_frame, uint8_t sequence)
{
    (void)rx_frame;

    RS485_Config_Snapshot_t snap = {0};
    const Application_Instance_t* app = Application_GetActive();
    snap.device_type           = (app != NULL) ? (uint8_t)app->type : 0;
    snap.fault_state           = (uint8_t)Fault_Manager_GetState();
    snap.fault_reason          = (uint8_t)Fault_Manager_GetReason();
    snap.filter_remaining_pct  = 0xFF;  /* Not tracked yet */
    snap.last_clean_unix_time  = 0;     /* Owned by clean tracker; future hookup */
    snap.total_dispensed_session = Dispenser_GetDispensedAmountML();
    snap.fw_version = ((uint32_t)FW_VERSION_MAJOR << 16) |
                      ((uint32_t)FW_VERSION_MINOR << 8)  |
                      (uint32_t)FW_VERSION_PATCH;

    rs485_send_response(sequence, RS485_CMD_CONFIG_RESPONSE, &snap, sizeof(snap));
    LOG_DEBUG_ADAPTER("[RS485_ADAPTER] Config snapshot sent\r\n");
    return true;
}

/* New Command Handlers -----------------------------------------------------*/

static bool cmd_heartbeat(const RS485_Frame_t *rx_frame, uint8_t sequence)
{
    (void)rx_frame;
    rs485_send_ack(sequence);
    return true;
}

static bool cmd_control_forward(const RS485_Frame_t *rx_frame, uint8_t sequence)
{
    return MyWota_Command_Processor_ExecuteRS485(rx_frame, sequence);
}

/* Helper Functions ----------------------------------------------------------*/

/**
 * @brief Send ACK response
 */
static void rs485_send_ack(uint8_t sequence)
{
    RS485_Frame_t* response_frame = RS485_GetScratchFrame();
    RS485_BuildFrame(response_frame, RS485_ADDR_MASTER, RS485_CMD_ACK, sequence, NULL, 0);
    RS485_SendFrame(response_frame);
}

/**
 * @brief Send NAK response
 */
static void rs485_send_nak(uint8_t sequence, RS485_NAK_Reason_t reason)
{
    RS485_Frame_t* response_frame = RS485_GetScratchFrame();
    uint8_t payload = (uint8_t)reason;
    RS485_BuildFrame(response_frame, RS485_ADDR_MASTER, RS485_CMD_NAK, sequence, &payload, 1);
    RS485_SendFrame(response_frame);
}

/**
 * @brief Send response with payload
 */
static void rs485_send_response(uint8_t sequence, RS485_Command_t command,
                                 const void* payload, uint16_t length)
{
    RS485_Frame_t* response_frame = RS485_GetScratchFrame();
    RS485_BuildFrame(response_frame, RS485_ADDR_MASTER, command, sequence, payload, length);
    RS485_SendFrame(response_frame);
}

/* Remote Command Capture Support */
#define REMOTE_CAPTURE_BUF_SIZE  2048
#define REMOTE_CHUNK_SIZE        128

static char s_remote_capture_buf[REMOTE_CAPTURE_BUF_SIZE];
static uint16_t s_remote_capture_len = 0;

static void remote_capture_handler(const char* message, size_t length, void* context)
{
    (void)context;
    size_t avail = REMOTE_CAPTURE_BUF_SIZE - s_remote_capture_len - 1;
    if (length > avail) length = avail;
    if (length > 0) {
        memcpy(s_remote_capture_buf + s_remote_capture_len, message, length);
        s_remote_capture_len += length;
        s_remote_capture_buf[s_remote_capture_len] = '\0';
    }
}

static bool cmd_debug_cmd(const RS485_Frame_t *rx_frame, uint8_t sequence)
{
    uint16_t cmd_len = rx_frame->header.length;
    char cmd_line[256];
    if (cmd_len >= sizeof(cmd_line)) {
        cmd_len = sizeof(cmd_line) - 1;
    }
    memcpy(cmd_line, rx_frame->payload, cmd_len);
    cmd_line[cmd_len] = '\0';

    s_remote_capture_buf[0] = '\0';
    s_remote_capture_len = 0;

#ifdef USE_DRIVERS_USB
    const CLI_Channel_t capture_channel = {
        .write = remote_capture_handler,
        .context = NULL
    };
    CLI_Execute(cmd_line, &capture_channel);
#endif

    uint8_t payload[3];
    payload[0] = s_remote_capture_len & 0xFF;
    payload[1] = (s_remote_capture_len >> 8) & 0xFF;
    
    uint16_t num_chunks = (s_remote_capture_len + REMOTE_CHUNK_SIZE - 1) / REMOTE_CHUNK_SIZE;
    payload[2] = (uint8_t)num_chunks;

    rs485_send_response(sequence, RS485_CMD_DEBUG_CMD, payload, sizeof(payload));

    LOG_DEBUG_ADAPTER("[RS485_ADAPTER] Debug cmd executed: '%s', len=%u, chunks=%u\r\n",
                      cmd_line, s_remote_capture_len, num_chunks);

    return true;
}

static bool cmd_debug_fetch(const RS485_Frame_t *rx_frame, uint8_t sequence)
{
    if (rx_frame->header.length < 1) {
        rs485_send_nak(sequence, RS485_NAK_INVALID_PARAM);
        return true;
    }

    uint8_t chunk_idx = rx_frame->payload[0];
    uint16_t offset = chunk_idx * REMOTE_CHUNK_SIZE;

    if (offset >= s_remote_capture_len) {
        rs485_send_response(sequence, RS485_CMD_DEBUG_FETCH, NULL, 0);
        return true;
    }

    uint16_t chunk_len = s_remote_capture_len - offset;
    if (chunk_len > REMOTE_CHUNK_SIZE) {
        chunk_len = REMOTE_CHUNK_SIZE;
    }

    rs485_send_response(sequence, RS485_CMD_DEBUG_FETCH, s_remote_capture_buf + offset, chunk_len);

    LOG_DEBUG_ADAPTER("[RS485_ADAPTER] Debug fetch sent chunk %u, len=%u\r\n", chunk_idx, chunk_len);

    return true;
}
