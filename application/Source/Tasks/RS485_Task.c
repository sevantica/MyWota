#include "RS485_Task.h"
#include "RS485_Protocol.h"
#include "RS485_Discovery.h"
#include "RS485_FW_Update.h"
#include "RS485_Slave_Common_Handlers.h"
#include "System_Command.h"
#include "RS485_Command_Adapter.h"
#include "RS485_Command_Interface.h"
#include "RTC_Manager.h"
#include "Heartbeat_Task.h"
#include "Task_Stack_Config.h"
#include "USB_Logging.h"
#include "USB_Command_Handler.h"
#include "Firmware_Version.h"
#include "Module_Interface.h"
#include "MyWota_System.h"
#include "Hardware_Access.h"
#include "Application_Interface.h"
#include "MIFARE_Card_Interface.h"
#include "MIFARE_Transaction_Core.h"
#include "PN532_Driver.h"
#include "Dispenser_Controller.h"
#include "Fault_Manager.h"
#include <string.h>
#include "Pico_HAL.h"

#if defined(PICO_BOARD) || defined(RP2040)
    #include "hardware/uart.h"
#endif

/* Configuration -------------------------------------------------------------*/
#define RS485_BAUDRATE              RS485_DEFAULT_BAUDRATE
#define RS485_MY_ADDRESS            0x01
#define RS485_FW_FLASH_OFFSET       0x00100000
#define RS485_FW_MAX_SIZE           (1024 * 1024)
#define RS485_REMOTE_CMD_MAX_LEN    127u
#define RS485_DEBUG_LOG_CHUNK_SIZE  256u

/* Private Variables ---------------------------------------------------------*/
static TaskHandle_t rs485_task_handle = NULL;
static bool rs485_initialized = false;

/* Static Allocation Buffers */
static StaticTask_t rs485_task_tcb;
static StackType_t rs485_task_stack[RS485_TASK_STACK_WORDS];

/* Log capture for RS485 debug commands */
#define RS485_SYSTEM_LOG_SIZE 1024
static struct {
    char buffer[RS485_SYSTEM_LOG_SIZE];
    uint16_t head;
    uint16_t tail;
    bool overflow;
} rs485_system_log = {0};

#define RS485_CMD_CAPTURE_SIZE 2048
static struct {
    char     buffer[RS485_CMD_CAPTURE_SIZE];
    uint16_t length;          /* Total captured output length */
    uint8_t  num_chunks;      /* Precomputed chunk count */
    bool     active;          /* Capture in progress */
    bool     ready;           /* Output captured, ready for fetch */
} rs485_cmd_capture = {0};

/* Private Function Prototypes -----------------------------------------------*/
static void RS485_Task(void* argument);
static void rs485_global_log_handler(const char* message, size_t length);
static void update_device_status(void);

/* Command Handlers */
static RS485_Result_t handle_sync_time(const RS485_Frame_t* req, RS485_Frame_t* resp);
static RS485_Result_t handle_fw_update(const RS485_Frame_t* req, RS485_Frame_t* resp);
static RS485_Result_t handle_debug_log(const RS485_Frame_t* req, RS485_Frame_t* resp);
static RS485_Result_t handle_debug_cmd(const RS485_Frame_t* req, RS485_Frame_t* resp);
static RS485_Result_t handle_debug_fetch(const RS485_Frame_t* req, RS485_Frame_t* resp);
static RS485_Result_t handle_trigger_clean(const RS485_Frame_t* req, RS485_Frame_t* resp);

/* ========================================================================== */
/*                            PUBLIC API                                      */
/* ========================================================================== */

void Task_Start_RS485_Task(void)
{
    if (rs485_task_handle != NULL) return;
    
    rs485_task_handle = xTaskCreateStatic(RS485_Task, "RS485_Task", RS485_TASK_STACK_WORDS, NULL, RS485_TASK_PRIORITY, rs485_task_stack, &rs485_task_tcb);
}

TaskHandle_t RS485_Task_GetHandle(void) { return rs485_task_handle; }
bool RS485_Task_IsReady(void) { return rs485_initialized; }

/* ========================================================================== */
/*                            TASK IMPLEMENTATION                             */
/* ========================================================================== */

static void RS485_Task(void* argument)
{
    (void)argument;
    
    /* 1. Initialize Hardware (UART0 for MyWota Slave) */
    /* This sets up the GPIO pins correctly */
    void* uart_handle = (void*)HAL_UART_Init(0, RS485_BAUDRATE, 0, 1, RS485_DATA_EN_PIN);
    if (uart_handle == NULL) {
        vTaskDelete(NULL);
        return;
    }
    
    /* 2. Initialize Standard RS485 Service (Slave Mode) */
    if (RS485_Slave_Init(uart_handle, RS485_MY_ADDRESS) != RS485_OK) {
        vTaskDelete(NULL);
        return;
    }
    
    /* 2b. Initialize Discovery so slave responds to CMD_DISCOVER broadcasts */
    RS485_Discovery_Slave_Init(RS485_DEVICE_TYPE_WATER_DISPENSER,
                               FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH);
    
    /* 3. Initialize Firmware Update Driver */
    RS485_FW_Update_Init(RS485_FW_FLASH_OFFSET, RS485_FW_MAX_SIZE);
    
    /* 3. Setup Debug Log Capture */
    USB_Log_SetOutputHandler(rs485_global_log_handler);
    
    /* 4. Register Command Handlers */
    RS485_Slave_RegisterHandler(RS485_CMD_SYNC_TIME, handle_sync_time);
    RS485_Slave_RegisterHandler(RS485_CMD_DEBUG_LOG, handle_debug_log);
    RS485_Slave_RegisterHandler(RS485_CMD_DEBUG_CMD, handle_debug_cmd);
    RS485_Slave_RegisterHandler(RS485_CMD_DEBUG_FETCH, handle_debug_fetch);
    RS485_Slave_RegisterHandler(RS485_CMD_TRIGGER_CLEAN, handle_trigger_clean);
    
    /* 5. Initialize Command Adapter (Legacy dispatcher fallback) */
    RS485_Command_Adapter_Init();
    
    rs485_initialized = true;
    USB_Log_Printf("[RS485] Slave Task Started (Addr: %d)\r\n", RS485_MY_ADDRESS);
    
    for(;;)
    {
        TASK_HEARTBEAT_EVERY_SECOND("RS485_Task");
        System_ReportTaskStatus(SYSTEM_TASK_ID_RS485, true);
        
        /* Update application status for polling */
        update_device_status();
        
        /* Process RS485 Service Logic (Standard Driver) */
        RS485_Slave_Process();
        
        /* Service timeouts */
        RS485_SlaveCommon_FW_Tick();
        
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ========================================================================== */
/*                            COMMAND HANDLERS                                */
/* ========================================================================== */

static RS485_Result_t handle_sync_time(const RS485_Frame_t* req, RS485_Frame_t* resp)
{
    if (req->header.length >= 4) {
        time_t unix_time;
        if (req->header.length == 8) {
            memcpy(&unix_time, req->payload, sizeof(time_t));
        } else {
            uint32_t t32;
            memcpy(&t32, req->payload, 4);
            unix_time = (time_t)t32;
        }
        
        System_Command_Request_t request = {
            .id = SYSTEM_CMD_ID_RTC_SYNC,
            .origin = SYSTEM_CMD_ORIGIN_RS485,
            .param1 = (uint32_t)unix_time,
        };
        System_Command_Status_t status = System_Command_Execute(&request, NULL);
        if (status == SYSTEM_CMD_STATUS_OK) {
            RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_ACK, req->header.sequence, NULL, 0);
            return RS485_OK;
        }
        uint8_t reason = (status == SYSTEM_CMD_STATUS_UNAUTHORIZED) ? RS485_NAK_NOT_AUTHORIZED : RS485_NAK_INVALID_PARAM;
        RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_NAK, req->header.sequence, &reason, 1);
        return RS485_OK;
    }
    
    uint8_t reason = RS485_NAK_INVALID_PARAM;
    RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_NAK, req->header.sequence, &reason, 1);
    return RS485_OK;
}

static RS485_Result_t handle_fw_update(const RS485_Frame_t* req, RS485_Frame_t* resp)
{
    if (System_Command_RequireAuth(SYSTEM_CMD_AUTH_ADMIN, "firmware update") != SYSTEM_CMD_STATUS_OK) {
        uint8_t reason = RS485_NAK_NOT_AUTHORIZED;
        RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_NAK, req->header.sequence, &reason, 1);
        return RS485_OK;
    }

    bool success = false;
    
    switch (req->header.command) {
        case RS485_CMD_FW_START:
            if (req->header.length >= 4) {
                uint32_t size;
                memcpy(&size, req->payload, 4);
                success = RS485_FW_Update_Start(size);
            }
            break;
            
        case RS485_CMD_FW_DATA:
            if (req->header.length >= sizeof(RS485_FW_Data_Header_t)) {
                const RS485_FW_Data_Header_t *header = (const RS485_FW_Data_Header_t *)req->payload;
                success = RS485_FW_Update_WriteData(header->offset,
                                                    req->payload + sizeof(RS485_FW_Data_Header_t),
                                                    header->length);
            }
            break;
            
        case RS485_CMD_FW_APPLY:
            success = RS485_FW_Update_Finalize();
            // Note: Reboot should be scheduled after response is sent
            // For now, assume it will be done via scheduled task or reset command
            break;
            
        case RS485_CMD_FW_VERIFY:
            RS485_FW_Update_Abort();
            success = true;
            break;
    }
    
    if (success) {
        RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_ACK, req->header.sequence, NULL, 0);
    } else {
        uint8_t reason = RS485_NAK_BUSY;
        RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_NAK, req->header.sequence, &reason, 1);
    }
    
    return RS485_OK;
}

static RS485_Result_t handle_debug_log(const RS485_Frame_t* req, RS485_Frame_t* resp)
{
    uint16_t log_len = 0;
    char temp_payload[RS485_DEBUG_LOG_CHUNK_SIZE];
    
    uint16_t available = (rs485_system_log.head >= rs485_system_log.tail) ? 
        (rs485_system_log.head - rs485_system_log.tail) : 
        (RS485_SYSTEM_LOG_SIZE - rs485_system_log.tail + rs485_system_log.head);
    
    log_len = (available > RS485_DEBUG_LOG_CHUNK_SIZE) ? RS485_DEBUG_LOG_CHUNK_SIZE : available;
    
    if (log_len > 0) {
        for(uint16_t i = 0; i < log_len; i++) {
            temp_payload[i] = rs485_system_log.buffer[rs485_system_log.tail];
            rs485_system_log.tail = (rs485_system_log.tail + 1) % RS485_SYSTEM_LOG_SIZE;
        }
        RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_DEBUG_LOG, req->header.sequence, temp_payload, log_len);
    } else {
        RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_ACK, req->header.sequence, NULL, 0);
    }
    
    return RS485_OK;
}

static RS485_Result_t handle_debug_cmd(const RS485_Frame_t* req, RS485_Frame_t* resp)
{
    if (req->header.length == 0) {
        uint8_t reason = RS485_NAK_INVALID_PARAM;
        RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_NAK, req->header.sequence, &reason, 1);
        return RS485_OK;
    }

    char cmd[RS485_REMOTE_CMD_MAX_LEN + 1u];
    size_t len = (req->header.length > RS485_REMOTE_CMD_MAX_LEN) ? RS485_REMOTE_CMD_MAX_LEN : req->header.length;
    memcpy(cmd, req->payload, len);
    cmd[len] = '\0';

    USB_Log_Printf("[RS485] Executing Remote Cmd: '%s'\r\n", cmd);

    /* Capture all command output into the dedicated buffer */
    rs485_cmd_capture.length = 0;
    rs485_cmd_capture.active = true;
    USB_Command_HandleString(cmd);
    rs485_cmd_capture.active = false;

    /* Compute chunk count (RS485_MAX_PAYLOAD bytes per chunk) */
    rs485_cmd_capture.num_chunks = (rs485_cmd_capture.length > 0)
        ? (uint8_t)((rs485_cmd_capture.length + RS485_MAX_PAYLOAD - 1) / RS485_MAX_PAYLOAD)
        : 0;
    rs485_cmd_capture.ready = true;

    /* Respond with metadata: total_size(2B LE) + num_chunks(1B) */
    uint8_t meta[3];
    meta[0] = (uint8_t)(rs485_cmd_capture.length & 0xFF);
    meta[1] = (uint8_t)(rs485_cmd_capture.length >> 8);
    meta[2] = rs485_cmd_capture.num_chunks;
    RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_DEBUG_CMD, req->header.sequence, meta, 3);
    return RS485_OK;
}

static RS485_Result_t handle_debug_fetch(const RS485_Frame_t* req, RS485_Frame_t* resp)
{
    /* Payload must contain 1 byte: chunk_index */
    if (req->header.length < 1 || !rs485_cmd_capture.ready) {
        uint8_t reason = RS485_NAK_INVALID_PARAM;
        RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_NAK, req->header.sequence, &reason, 1);
        return RS485_OK;
    }

    uint8_t chunk_idx = req->payload[0];
    if (chunk_idx >= rs485_cmd_capture.num_chunks) {
        uint8_t reason = RS485_NAK_INVALID_PARAM;
        RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_NAK, req->header.sequence, &reason, 1);
        return RS485_OK;
    }

    uint16_t offset = (uint16_t)chunk_idx * RS485_MAX_PAYLOAD;
    uint16_t remaining = rs485_cmd_capture.length - offset;
    uint16_t chunk_len = (remaining > RS485_MAX_PAYLOAD) ? RS485_MAX_PAYLOAD : remaining;

    RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_DEBUG_FETCH,
                     req->header.sequence,
                     &rs485_cmd_capture.buffer[offset], chunk_len);
    return RS485_OK;
}

/**
 * @brief Handle CCH-issued TRIGGER_CLEAN command.
 *
 * Payload: RS485_TriggerClean_Payload_t { uint16_t target_volume_ml;
 *                                         uint16_t max_duration_sec; }
 * - target_volume_ml == 0 → abort any in-progress self-clean
 * - non-zero            → start a clean cycle with given parameters
 *                         (0 in either field uses config defaults)
 *
 * Replies ACK on success, NAK with reason byte otherwise.
 */
static RS485_Result_t handle_trigger_clean(const RS485_Frame_t* req, RS485_Frame_t* resp)
{
    if (req->header.length < sizeof(RS485_TriggerClean_Payload_t)) {
        uint8_t reason = RS485_NAK_INVALID_PARAM;
        RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_NAK, req->header.sequence, &reason, 1);
        return RS485_OK;
    }

    RS485_TriggerClean_Payload_t payload;
    memcpy(&payload, req->payload, sizeof(payload));

    /* volume == 0 means "abort" */
    if (payload.target_volume_ml == 0) {
        System_Command_Request_t request = {
            .id = SYSTEM_CMD_ID_CLEAN_STOP,
            .origin = SYSTEM_CMD_ORIGIN_RS485,
        };
        if (System_Command_Execute(&request, NULL) == SYSTEM_CMD_STATUS_OK) {
            RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_ACK, req->header.sequence, NULL, 0);
        } else {
            uint8_t reason = RS485_NAK_NOT_AUTHORIZED;
            RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_NAK, req->header.sequence, &reason, 1);
        }
        return RS485_OK;
    }

    System_Command_Request_t request = {
        .id = SYSTEM_CMD_ID_CLEAN_START,
        .origin = SYSTEM_CMD_ORIGIN_RS485,
        .param1 = payload.target_volume_ml,
        .param2 = payload.max_duration_sec,
    };
    System_Command_Status_t status = System_Command_Execute(&request, NULL);
    if (status == SYSTEM_CMD_STATUS_OK) {
        RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_ACK, req->header.sequence, NULL, 0);
    } else {
        uint8_t reason = (status == SYSTEM_CMD_STATUS_UNAUTHORIZED) ? RS485_NAK_NOT_AUTHORIZED : RS485_NAK_BUSY;
        RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_NAK, req->header.sequence, &reason, 1);
    }
    return RS485_OK;
}

/* ========================================================================== */
/*                            HELPER FUNCTIONS                                */
/* ========================================================================== */

/**
 * @brief Update standard device status structure from application variables
 */

// ...

static void update_device_status(void)
{
    RS485_Device_Status_t status = {0};
    
    /* Get application instance */
    const Application_Instance_t* app = Application_GetActive();
    
    if (app != NULL && app->callbacks != NULL) {
        status.device_type = app->type;
        
        if (app->callbacks->get_state != NULL) {
            status.state = app->callbacks->get_state();
        }
        
        if (app->callbacks->get_primary_balance != NULL) {
            status.balance = app->callbacks->get_primary_balance();
        }
        
        if (app->callbacks->is_operation_active != NULL) {
            if (app->callbacks->is_operation_active()) {
                status.flags |= RS485_STATUS_FLAG_DISPENSING;
            }
        }
    }
    
    /* Get last error from Dispenser Controller directly */
    status.error_code = Dispenser_GetLastError();

    /* Self-clean reporting (CCH-orchestrated periodic flush) */
    if (Dispenser_IsSelfCleaning()) {
        status.flags |= RS485_STATUS_FLAG_SELF_CLEANING;
    }
    status.last_clean_unix_time = Dispenser_GetLastCleanUnixTime();

    /* Fault state machine. Filter life is tracked at CCH (shared filter per site). */
    status.fault_state          = (uint8_t)Fault_Manager_GetState();
    status.fault_reason         = (uint8_t)Fault_Manager_GetReason();
    status.filter_remaining_pct = 0xFF;  /* N/A from slave; CCH derives it */

    /* Pump request (CCH aggregates across all slaves to drive shared pumps) */
    Dispenser_GetPeripheralRequest(&status.peripheral_request_id, &status.peripheral_request_level);

    uint32_t admin_remaining_ms = System_Command_GetAdminRemainingMs();
    if (admin_remaining_ms > 0u) {
        status.flags |= RS485_STATUS_FLAG_ADMIN_AUTH;
        uint32_t admin_remaining_sec = (admin_remaining_ms + 999u) / 1000u;
        status.reserved_v2 = (admin_remaining_sec > 255u) ? 255u : (uint8_t)admin_remaining_sec;
    }
    
    /* Get card information */
    status.flags |= (MIFARE_IsCardPresent() ? RS485_STATUS_FLAG_CARD_PRESENT : 0);
    
    if (MIFARE_IsCardReady()) {
        PN532_CardInfo_t card_info;
        if (MIFARE_GetCurrentCardInfo(&card_info)) {
            uint8_t uid_len = (card_info.uid_length <= 7) ? card_info.uid_length : 7;
            memcpy(status.card_uid, card_info.uid, uid_len);
            status.card_uid_length = uid_len;
        }
    }
    
    /* Push to standard service */
    RS485_Slave_SetStatus(&status);
}

static void rs485_global_log_handler(const char* message, size_t length)
{
    /* Append to circular buffer */
    for (size_t i = 0; i < length; i++) {
        rs485_system_log.buffer[rs485_system_log.head] = message[i];
        rs485_system_log.head = (rs485_system_log.head + 1) % RS485_SYSTEM_LOG_SIZE;
        if (rs485_system_log.head == rs485_system_log.tail) {
            rs485_system_log.tail = (rs485_system_log.tail + 1) % RS485_SYSTEM_LOG_SIZE;
            rs485_system_log.overflow = true;
        }
    }
    
    /* Capture for command response */
    if (rs485_cmd_capture.active) {
        /* Only capture output generated by THIS task (the one running the command) */
        /* This prevents logs from other tasks (e.g. LCD, Dispenser) from corrupting the command output */
        if (xTaskGetCurrentTaskHandle() == rs485_task_handle) {
            size_t space = sizeof(rs485_cmd_capture.buffer) - rs485_cmd_capture.length - 1;
            size_t to_copy = (length < space) ? length : space;
            if (to_copy > 0) {
                memcpy(&rs485_cmd_capture.buffer[rs485_cmd_capture.length], message, to_copy);
                rs485_cmd_capture.length += to_copy;
                rs485_cmd_capture.buffer[rs485_cmd_capture.length] = '\0';
            }
        }
    }
}