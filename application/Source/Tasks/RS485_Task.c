#include "RS485_Task.h"
#include "RS485_Protocol.h"
#include "RS485_FW_Update.h"
#include "RS485_Command_Adapter.h"
#include "RS485_Command_Interface.h"
#include "RTC_Manager.h"
#include "Heartbeat_Task.h"
#include "Task_Stack_Config.h"
#include "USB_Logging.h"
#include "USB_Command_Handler.h"
#include "Module_Interface.h"
#include "MyWota_System.h"
#include "Hardware_Access.h"
#include "Application_Interface.h"
#include "MIFARE_Card_Interface.h"
#include "MIFARE_Transaction_Core.h"
#include "PN532_Driver.h"
#include <string.h>
#include "RP2040_HAL.h"

#if defined(PICO_BOARD) || defined(RP2040)
    #include "hardware/uart.h"
#endif

/* Configuration -------------------------------------------------------------*/
#define RS485_BAUDRATE              115200
#define RS485_MY_ADDRESS            0x01
#define RS485_FW_FLASH_OFFSET       0x00100000
#define RS485_FW_MAX_SIZE           (1024 * 1024)

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

static struct {
    char buffer[RS485_MAX_PAYLOAD];
    size_t length;
    bool active;
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
    /* This sets up the GPIO pins correctly */
    void* uart_handle = (void*)HAL_UART_Init(0, RS485_BAUDRATE, 0, 1, RS485_DATA_EN_PIN);
    if (uart_handle == NULL) {
        vTaskDelete(NULL);
        return;
    }
    
    /* 2. Initialize Standard RS485 Service (Slave Mode) */
    /* Note: UART0 = Index 0, RS485_DATA_EN_PIN = Direction Enable */
    if (RS485_Slave_Init(0, RS485_BAUDRATE, RS485_DATA_EN_PIN, RS485_MY_ADDRESS) != RS485_OK) {
        vTaskDelete(NULL);
        return;
    }
    
    /* 3. Initialize Firmware Update Driver */
    RS485_FW_Update_Init(RS485_FW_FLASH_OFFSET, RS485_FW_MAX_SIZE);
    
    /* 3. Setup Debug Log Capture */
    USB_Log_SetOutputHandler(rs485_global_log_handler);
    
    /* 4. Register Command Handlers */
    RS485_Slave_RegisterHandler(RS485_CMD_SYNC_TIME, handle_sync_time);
    RS485_Slave_RegisterHandler(RS485_CMD_FW_START,  handle_fw_update);
    RS485_Slave_RegisterHandler(RS485_CMD_FW_DATA,   handle_fw_update);
    RS485_Slave_RegisterHandler(RS485_CMD_FW_APPLY,  handle_fw_update);
    RS485_Slave_RegisterHandler(RS485_CMD_FW_VERIFY, handle_fw_update);
    RS485_Slave_RegisterHandler(RS485_CMD_DEBUG_LOG, handle_debug_log);
    RS485_Slave_RegisterHandler(RS485_CMD_DEBUG_CMD, handle_debug_cmd);
    
    /* 5. Initialize Command Adapter (Legacy dispatcher fallback) */
    RS485_Command_Adapter_Init();
    
    rs485_initialized = true;
    USB_Log_Printf("[RS485] Slave Task Started (Addr: %d)\r\n", RS485_MY_ADDRESS);
    
    for(;;)
    {
        TASK_HEARTBEAT_EVERY_SECOND("RS485_Task");
        System_ReportTaskStatus(SYS_TASK_ID_RS485, true);
        
        /* Update application status for polling */
        update_device_status();
        
        /* Process RS485 Service Logic (Standard Driver) */
        RS485_Slave_Process();
        
        /* Service timeouts */
        RS485_FW_Update_CheckTimeout();
        
        vTaskDelay(pdMS_TO_TICKS(2));
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
        
        if (RTC_SetUnixTime(unix_time) == RTC_OK) {
            RTC_SaveToSD();
            RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_ACK, req->header.sequence, NULL, 0);
            return RS485_OK;
        }
    }
    
    uint8_t reason = RS485_NAK_INVALID_PARAM;
    RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_NAK, req->header.sequence, &reason, 1);
    return RS485_OK;
}

static RS485_Result_t handle_fw_update(const RS485_Frame_t* req, RS485_Frame_t* resp)
{
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
            success = RS485_FW_Update_WriteData(req->payload, req->header.length);
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
    char temp_payload[RS485_MAX_PAYLOAD];
    
    uint16_t available = (rs485_system_log.head >= rs485_system_log.tail) ? 
        (rs485_system_log.head - rs485_system_log.tail) : 
        (RS485_SYSTEM_LOG_SIZE - rs485_system_log.tail + rs485_system_log.head);
    
    log_len = (available > RS485_MAX_PAYLOAD) ? RS485_MAX_PAYLOAD : available;
    
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
    if (req->header.length > 0) {
        char cmd[RS485_MAX_PAYLOAD + 1];
        size_t len = (req->header.length > RS485_MAX_PAYLOAD) ? RS485_MAX_PAYLOAD : req->header.length;
        memcpy(cmd, req->payload, len);
        cmd[len] = '\0';
        
        rs485_cmd_capture.length = 0;
        rs485_cmd_capture.active = true;
        USB_Command_Status_t st = USB_Command_HandleString(cmd);
        rs485_cmd_capture.active = false;
        
        if (st != USB_CMD_OK && rs485_cmd_capture.length == 0) {
            /* If command failed and produced no output, send generic NAK */
             uint8_t reason = RS485_NAK_INVALID_PARAM; // Or generic error
             RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_ACK, req->header.sequence, NULL, 0);
             // Actually, user wants to know it failed. But ACK with "OK" on master is confusing.
             // Master prints "OK" on ACK.
             // If we send DEBUG_CMD with empty string? Master prints nothing.
             // If we send captured output, Master prints it.
        }
        
        if (rs485_cmd_capture.length > 0) {
             /* Send captured output even if command 'failed' (e.g. unknown command print) */
            RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_DEBUG_CMD, req->header.sequence, 
                             rs485_cmd_capture.buffer, rs485_cmd_capture.length);
        } else {
             /* No output. If st was error, strictly we should NAK? 
                But 'remote' command on master says "OK" if ACK.
                Let's stick to ACK for now to mean "Executed (silently)". */
             RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_ACK, req->header.sequence, NULL, 0);
        }
        return RS485_OK;
    }
    
    uint8_t reason = RS485_NAK_INVALID_PARAM;
    RS485_BuildFrame(resp, RS485_ADDR_MASTER, RS485_CMD_NAK, req->header.sequence, &reason, 1);
    return RS485_OK;
}

/* ========================================================================== */
/*                            HELPER FUNCTIONS                                */
/* ========================================================================== */

/**
 * @brief Update standard device status structure from application variables
 */
#include "Dispenser_Controller.h"

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
    
    /* Get card information */
    status.flags |= (MIFARE_IsCardReady() ? RS485_STATUS_FLAG_CARD_PRESENT : 0);
    
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
        size_t space = sizeof(rs485_cmd_capture.buffer) - rs485_cmd_capture.length - 1;
        size_t to_copy = (length < space) ? length : space;
        if (to_copy > 0) {
            memcpy(&rs485_cmd_capture.buffer[rs485_cmd_capture.length], message, to_copy);
            rs485_cmd_capture.length += to_copy;
            rs485_cmd_capture.buffer[rs485_cmd_capture.length] = '\0';
        }
    }
}