/**
 ******************************************************************************
 * @file    RS485_Task.c
 * @brief   RS485 Communication Task Implementation
 * 
 * @attention
 * Copyright (c) Sevantica 2025
 * 
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "RS485_Task.h"
#include "RS485_Protocol.h"
#include "RS485_File_Transfer.h"
#include "RS485_Command_Interface.h"
#include "RTC_Manager.h"
#include "Heartbeat_Task.h"
#include "Task_Stack_Config.h"
#include "USB_Logging.h"
#include "USB_Command_Handler.h"
#include "System.h"
#include "Hardware_Access.h"
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include <string.h>

/* Configuration -------------------------------------------------------------*/
#define RS485_SLAVE_ADDRESS         0x01    /* This device's RS485 address */
#define RS485_BAUDRATE              115200  /* 115200 baud */
#define RS485_UART_INSTANCE         uart0   /* UART0 */
#define RS485_RX_BUFFER_SIZE        1024
#define RS485_FRAME_TIMEOUT_MS      100     /* Frame reception timeout */

/* Firmware Update Configuration */
#define FW_UPDATE_BUFFER_SIZE       4096    /* 4KB flash sector size */
#define FW_UPDATE_FLASH_OFFSET      0x00100000  /* 1MB offset (Application start) */
#define FW_UPDATE_MAX_SIZE          (1024 * 1024)  /* 1MB max firmware size */
#define FW_UPDATE_TIMEOUT_MS        60000   /* 60 second timeout */

/* Logging Configuration -----------------------------------------------------*/
#define LOG_DEBUG_RS485_EN          1
#define LOG_ERROR_RS485_EN          1

#if LOG_DEBUG_RS485_EN
    #define LOG_DEBUG_RS485(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_DEBUG_RS485(...)
#endif

#if LOG_ERROR_RS485_EN
    #define LOG_ERROR_RS485(...) USB_Log_Printf(__VA_ARGS__)
#else
    #define LOG_ERROR_RS485(...)
#endif

/* Private Variables ---------------------------------------------------------*/
static TaskHandle_t rs485_task_handle = NULL;
static bool rs485_initialized = false;
static uint8_t rx_buffer[RS485_RX_BUFFER_SIZE];
static uint16_t rx_index = 0;
static uint32_t last_rx_time = 0;
static App_GPIO_Pins_t app_pins;

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

/**
 * @brief Global log handler for RS485
 * @details Captures all system logs into a circular buffer and optionally a command capture buffer
 */
static void rs485_global_log_handler(const char* message, size_t length)
{
    // 1. Append to circular system log buffer
    for (size_t i = 0; i < length; i++) {
        rs485_system_log.buffer[rs485_system_log.head] = message[i];
        rs485_system_log.head = (rs485_system_log.head + 1) % RS485_SYSTEM_LOG_SIZE;
        
        // If head catches tail, move tail forward (drop oldest)
        if (rs485_system_log.head == rs485_system_log.tail) {
            rs485_system_log.tail = (rs485_system_log.tail + 1) % RS485_SYSTEM_LOG_SIZE;
            rs485_system_log.overflow = true;
        }
    }
    
    // 2. Append to command capture buffer if a debug command is active
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

/* Firmware Update State */
static bool fw_update_active = false;
static uint8_t fw_update_buffer[FW_UPDATE_BUFFER_SIZE] __attribute__((aligned(4)));
static uint32_t fw_update_buffer_index = 0;
static uint32_t fw_update_total_bytes = 0;
static uint32_t fw_update_expected_size = 0;
static uint32_t fw_update_last_activity = 0;

/* Private Function Prototypes -----------------------------------------------*/
static void RS485_Task(void* argument);
static void rs485_hardware_init(void);
static void rs485_set_transmit_mode(bool enable);
static bool rs485_receive_frame(RS485_Frame_t *frame, uint32_t timeout_ms);
static void rs485_send_frame(const RS485_Frame_t *frame);
static void rs485_send_ack(uint8_t sequence);
static void rs485_send_nak(uint8_t sequence, RS485_NAK_Reason_t reason);
static void rs485_handle_command(const RS485_Frame_t *rx_frame);
static bool fw_update_start(uint32_t expected_size);
static bool fw_update_write_data(const uint8_t *data, uint16_t length);
static bool fw_update_finalize(void);
static void fw_update_abort(void);
static void fw_update_check_timeout(void);

/* ========================================================================== */
/*                            PUBLIC API                                      */
/* ========================================================================== */

/**
 * @brief Start RS485 communication task
 */
void Task_Start_RS485_Task(void)
{
    if (rs485_task_handle != NULL) {
        LOG_ERROR_RS485("[RS485] Task already started\r\n");
        return;
    }
    
    BaseType_t result = xTaskCreate(
        RS485_Task,
        "RS485_Task",
        RS485_TASK_STACK_WORDS,
        NULL,
        RS485_TASK_PRIORITY,
        &rs485_task_handle
    );
    
    if (result != pdPASS) {
        LOG_ERROR_RS485("[RS485] Failed to create task\r\n");
        rs485_task_handle = NULL;
    }
}

/**
 * @brief Get RS485 task handle
 */
TaskHandle_t RS485_Task_GetHandle(void)
{
    return rs485_task_handle;
}

/**
 * @brief Check if RS485 is ready
 */
bool RS485_Task_IsReady(void)
{
    return rs485_initialized;
}

/**
 * @brief Send RS485 frame (public API for command adaptors)
 */
void RS485_SendFrame(const RS485_Frame_t *frame)
{
    rs485_send_frame(frame);
}

/* ========================================================================== */
/*                            TASK IMPLEMENTATION                             */
/* ========================================================================== */

/**
 * @brief RS485 communication task
 */
static void RS485_Task(void* argument)
{
    (void)argument;
    
    LOG_DEBUG_RS485("[RS485] Task started\r\n");
    
    // Initialize hardware
    rs485_hardware_init();
    
    // Initialize file transfer module
    RS485_FileTransfer_Init();
    
    // Register global log handler to capture all system activity
    USB_Log_SetOutputHandler(rs485_global_log_handler);
    
    rs485_initialized = true;
    LOG_DEBUG_RS485("[RS485] Initialized as slave address 0x%02X\r\n", RS485_SLAVE_ADDRESS);
    
    RS485_Frame_t rx_frame;
    
    for(;;)
    {
        TASK_HEARTBEAT_EVERY_SECOND("RS485_Task");
        System_ReportTaskStatus(SYSTEM_TASK_ID_RS485, true);
        
        // Check for firmware update timeout
        fw_update_check_timeout();
        
        // Check for file transfer timeout
        RS485_FileTransfer_CheckTimeout();
        
        // Wait for incoming frame
        if (rs485_receive_frame(&rx_frame, 100)) {
            // Validate frame
            RS485_Result_t result = RS485_ValidateFrame(&rx_frame);
            
            if (result == RS485_OK) {
                // Check if addressed to us or broadcast
                if (rx_frame.header.address == RS485_SLAVE_ADDRESS || 
                    rx_frame.header.address == RS485_ADDR_BROADCAST) {
                    rs485_handle_command(&rx_frame);
                }
            } else {
                LOG_ERROR_RS485("[RS485] Invalid frame: %s\r\n", RS485_GetResultString(result));
                rs485_send_nak(rx_frame.header.sequence, RS485_NAK_CRC_ERROR);
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ========================================================================== */
/*                         HARDWARE LAYER                                     */
/* ========================================================================== */

/**
 * @brief Initialize RS485 hardware (UART0 + DE pin)
 */
static void rs485_hardware_init(void)
{
    app_pins = Get_App_GPIO_Pins();
    
    // Initialize UART0 (already done by Init_Hardware_Layer, but ensure it's configured)
    uart_init(RS485_UART_INSTANCE, RS485_BAUDRATE);
    uart_set_format(RS485_UART_INSTANCE, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(RS485_UART_INSTANCE, true);
    
    // Data Enable pin already initialized by Hardware_Access
    rs485_set_transmit_mode(false);  // Start in receive mode
    
    LOG_DEBUG_RS485("[RS485] Hardware initialized (UART0 @ %d baud, DE pin: GPIO%d)\r\n", 
                    RS485_BAUDRATE, app_pins.rs485_data_en_pin);
}

/**
 * @brief Set RS485 transceiver mode
 */
static void rs485_set_transmit_mode(bool enable)
{
    gpio_put(app_pins.rs485_data_en_pin, enable);
    if (enable) {
        busy_wait_us(50);  // Short delay for transceiver to switch
    }
}

/**
 * @brief Receive RS485 frame with timeout
 */
static bool rs485_receive_frame(RS485_Frame_t *frame, uint32_t timeout_ms)
{
    uint32_t start_time = xTaskGetTickCount();
    rx_index = 0;
    bool found_sync = false;
    
    while ((xTaskGetTickCount() - start_time) < pdMS_TO_TICKS(timeout_ms)) {
        // Check if data available
        if (!uart_is_readable(RS485_UART_INSTANCE)) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        
        uint8_t byte = uart_getc(RS485_UART_INSTANCE);
        
        // Look for sync byte
        if (!found_sync) {
            if (byte == RS485_SYNC_BYTE) {
                rx_buffer[0] = byte;
                rx_index = 1;
                found_sync = true;
                last_rx_time = xTaskGetTickCount();
            }
            continue;
        }
        
        // Receive rest of frame
        rx_buffer[rx_index++] = byte;
        last_rx_time = xTaskGetTickCount();
        
        // Check if we have header (6 bytes: sync + addr + cmd + seq + len[2])
        if (rx_index >= 6) {
            uint16_t payload_len = rx_buffer[4] | (rx_buffer[5] << 8);
            uint16_t total_len = 6 + payload_len + 2;  // Header + payload + CRC
            
            if (rx_index >= total_len) {
                // Complete frame received
                memcpy(&frame->header, rx_buffer, sizeof(RS485_Header_t));
                if (payload_len > 0) {
                    memcpy(frame->payload, &rx_buffer[6], payload_len);
                }
                frame->crc = rx_buffer[total_len - 2] | (rx_buffer[total_len - 1] << 8);
                return true;
            }
        }
        
        // Check for inter-byte timeout
        if ((xTaskGetTickCount() - last_rx_time) > pdMS_TO_TICKS(RS485_FRAME_TIMEOUT_MS)) {
            LOG_ERROR_RS485("[RS485] Frame timeout\r\n");
            return false;
        }
    }
    
    return false;
}

/**
 * @brief Send RS485 frame
 */
static void rs485_send_frame(const RS485_Frame_t *frame)
{
    uint16_t total_len = sizeof(RS485_Header_t) + frame->header.length + 2;
    uint8_t tx_buffer[RS485_MAX_FRAME_SIZE];
    
    // Build transmit buffer
    memcpy(tx_buffer, &frame->header, sizeof(RS485_Header_t));
    if (frame->header.length > 0) {
        memcpy(&tx_buffer[sizeof(RS485_Header_t)], frame->payload, frame->header.length);
    }
    tx_buffer[total_len - 2] = frame->crc & 0xFF;
    tx_buffer[total_len - 1] = (frame->crc >> 8) & 0xFF;
    
    // Switch to transmit mode
    rs485_set_transmit_mode(true);
    
    // Send data
    uart_write_blocking(RS485_UART_INSTANCE, tx_buffer, total_len);
    uart_tx_wait_blocking(RS485_UART_INSTANCE);
    
    // Switch back to receive mode
    rs485_set_transmit_mode(false);
}

/**
 * @brief Send ACK response
 */
static void rs485_send_ack(uint8_t sequence)
{
    RS485_Frame_t ack_frame;
    RS485_BuildFrame(&ack_frame, RS485_ADDR_MASTER, RS485_CMD_ACK, sequence, NULL, 0);
    rs485_send_frame(&ack_frame);
}

/**
 * @brief Send NAK response
 */
static void rs485_send_nak(uint8_t sequence, RS485_NAK_Reason_t reason)
{
    RS485_Frame_t nak_frame;
    uint8_t payload = (uint8_t)reason;
    RS485_BuildFrame(&nak_frame, RS485_ADDR_MASTER, RS485_CMD_NAK, sequence, &payload, 1);
    rs485_send_frame(&nak_frame);
}

/* ========================================================================== */
/*                         COMMAND HANDLERS                                   */
/* ========================================================================== */

/**
 * @brief Handle received command
 */
static void rs485_handle_command(const RS485_Frame_t *rx_frame)
{
    switch (rx_frame->header.command) {
        
        case RS485_CMD_PING:
            LOG_DEBUG_RS485("[RS485] PING received\r\n");
            rs485_send_ack(rx_frame->header.sequence);
            break;
        
        case RS485_CMD_SYNC_TIME: {
            // Payload = Unix timestamp (4 or 8 bytes)
            if (rx_frame->header.length >= 4) {
                time_t unix_time;
                if (rx_frame->header.length == 8) {
                    // 64-bit time_t
                    memcpy(&unix_time, rx_frame->payload, sizeof(time_t));
                } else {
                    // 32-bit timestamp (backwards compatibility)
                    uint32_t time32;
                    memcpy(&time32, rx_frame->payload, sizeof(uint32_t));
                    unix_time = (time_t)time32;
                }
                
                RTC_Status_t rtc_status = RTC_SetUnixTime(unix_time);
                if (rtc_status == RTC_OK) {
                    LOG_DEBUG_RS485("[RS485] TIME_SYNC: Set to %ld (Unix timestamp)\r\n", 
                                   (long)unix_time);
                    
                    // Immediately save to SD
                    RTC_SaveToSD();
                    
                    rs485_send_ack(rx_frame->header.sequence);
                } else {
                    LOG_ERROR_RS485("[RS485] TIME_SYNC failed: %s\r\n", 
                                   RTC_GetStatusString(rtc_status));
                    rs485_send_nak(rx_frame->header.sequence, RS485_NAK_INVALID_PARAM);
                }
            } else {
                LOG_ERROR_RS485("[RS485] TIME_SYNC: Invalid payload length\r\n");
                rs485_send_nak(rx_frame->header.sequence, RS485_NAK_INVALID_PARAM);
            }
            break;
        }
        
        case RS485_CMD_FW_START: {
            // Payload = 4 bytes (expected firmware size in little-endian)
            if (rx_frame->header.length >= 4) {
                uint32_t expected_size;
                memcpy(&expected_size, rx_frame->payload, sizeof(uint32_t));
                
                LOG_DEBUG_RS485("[RS485] FW_START: Expected size %lu bytes\r\n", expected_size);
                
                if (fw_update_start(expected_size)) {
                    rs485_send_ack(rx_frame->header.sequence);
                } else {
                    LOG_ERROR_RS485("[RS485] FW start failed\r\n");
                    rs485_send_nak(rx_frame->header.sequence, RS485_NAK_NOT_READY);
                }
            } else {
                LOG_ERROR_RS485("[RS485] FW_START: Invalid payload\r\n");
                rs485_send_nak(rx_frame->header.sequence, RS485_NAK_INVALID_PARAM);
            }
            break;
        }
        
        case RS485_CMD_FW_DATA:
            if (fw_update_write_data(rx_frame->payload, rx_frame->header.length)) {
                rs485_send_ack(rx_frame->header.sequence);
            } else {
                LOG_ERROR_RS485("[RS485] FW data write failed\r\n");
                rs485_send_nak(rx_frame->header.sequence, RS485_NAK_BUSY);
            }
            break;
        
        case RS485_CMD_FW_APPLY:
            LOG_DEBUG_RS485("[RS485] FW_APPLY - Finalizing update\r\n");
            if (fw_update_finalize()) {
                rs485_send_ack(rx_frame->header.sequence);
                vTaskDelay(pdMS_TO_TICKS(100));  // Give time for ACK to send
                LOG_DEBUG_RS485("[RS485] Rebooting to apply firmware...\r\n");
                watchdog_reboot(0, 0, 10);  // Reboot in 10ms
            } else {
                LOG_ERROR_RS485("[RS485] FW finalize failed\r\n");
                rs485_send_nak(rx_frame->header.sequence, RS485_NAK_BUSY);
            }
            break;
        
        case RS485_CMD_FW_VERIFY:
            // Abort current firmware update
            LOG_DEBUG_RS485("[RS485] FW_ABORT\r\n");
            fw_update_abort();
            rs485_send_ack(rx_frame->header.sequence);
            break;
            
        case RS485_CMD_DEBUG_LOG: {
            // Return accumulated system logs
            uint16_t log_len = 0;
            char temp_payload[RS485_MAX_PAYLOAD];
            
            // Calculate how much we can send
            uint16_t available;
            if (rs485_system_log.head >= rs485_system_log.tail) {
                available = rs485_system_log.head - rs485_system_log.tail;
            } else {
                available = RS485_SYSTEM_LOG_SIZE - rs485_system_log.tail + rs485_system_log.head;
            }
            
            log_len = (available > RS485_MAX_PAYLOAD) ? RS485_MAX_PAYLOAD : available;
            
            if (log_len > 0) {
                // Copy from circular buffer to linear response payload
                for (uint16_t i = 0; i < log_len; i++) {
                    temp_payload[i] = rs485_system_log.buffer[rs485_system_log.tail];
                    rs485_system_log.tail = (rs485_system_log.tail + 1) % RS485_SYSTEM_LOG_SIZE;
                }
                
                RS485_Frame_t response;
                RS485_BuildFrame(&response, RS485_ADDR_MASTER, RS485_CMD_DEBUG_LOG,
                                 rx_frame->header.sequence, temp_payload, log_len);
                rs485_send_frame(&response);
            } else {
                // No logs available
                rs485_send_ack(rx_frame->header.sequence);
            }
            break;
        }
        
        case RS485_CMD_DEBUG_CMD: {
            if (rx_frame->header.length > 0) {
                char cmd_temp[RS485_MAX_PAYLOAD + 1];
                memcpy(cmd_temp, rx_frame->payload, rx_frame->header.length);
                cmd_temp[rx_frame->header.length] = '\0';
                
                // Clear and enable command output capture
                rs485_cmd_capture.length = 0;
                rs485_cmd_capture.active = true;
                
                USB_Command_Status_t status = USB_Command_HandleString(cmd_temp);
                
                // Disable capture
                rs485_cmd_capture.active = false;
                
                if (status == USB_CMD_OK) {
                    // Send captured logs as response if any, otherwise ACK
                    if (rs485_cmd_capture.length > 0) {
                        RS485_Frame_t response;
                        RS485_BuildFrame(&response, RS485_ADDR_MASTER, RS485_CMD_DEBUG_CMD,
                                         rx_frame->header.sequence, 
                                         rs485_cmd_capture.buffer, 
                                         (uint16_t)rs485_cmd_capture.length);
                        rs485_send_frame(&response);
                    } else {
                        rs485_send_ack(rx_frame->header.sequence);
                    }
                } else {
                    rs485_send_nak(rx_frame->header.sequence, RS485_NAK_INVALID_CMD);
                }
            } else {
                rs485_send_nak(rx_frame->header.sequence, RS485_NAK_INVALID_PARAM);
            }
            break;
        }
        
        default:
            // Try project-specific command handlers from adaptor
            if (RS485_DispatchCommand(rx_frame, rx_frame->header.sequence)) {
                // Command was handled by adaptor
                break;
            }
            
            // Unknown command - not handled by core or adaptor
            LOG_ERROR_RS485("[RS485] Unknown command: 0x%02X\r\n", rx_frame->header.command);
            rs485_send_nak(rx_frame->header.sequence, RS485_NAK_INVALID_CMD);
            break;
    }
}
/* ========================================================================== */
/*                      FIRMWARE UPDATE IMPLEMENTATION                        */
/* ========================================================================== */

/**
 * @brief Start firmware update process
 * @param expected_size Total firmware size in bytes
 * @return true if started successfully
 */
static bool fw_update_start(uint32_t expected_size)
{
    // Check if already in progress
    if (fw_update_active) {
        LOG_ERROR_RS485("[FW_UPDATE] Update already in progress\r\n");
        return false;
    }
    
    // Validate size
    if (expected_size == 0 || expected_size > FW_UPDATE_MAX_SIZE) {
        LOG_ERROR_RS485("[FW_UPDATE] Invalid size: %lu bytes\r\n", expected_size);
        return false;
    }
    
    // Initialize state
    fw_update_active = true;
    fw_update_buffer_index = 0;
    fw_update_total_bytes = 0;
    fw_update_expected_size = expected_size;
    fw_update_last_activity = xTaskGetTickCount();
    
    LOG_DEBUG_RS485("[FW_UPDATE] Started - expecting %lu bytes\r\n", expected_size);
    return true;
}

/**
 * @brief Write firmware data chunk
 * @param data Firmware data
 * @param length Data length
 * @return true if written successfully
 */
static bool fw_update_write_data(const uint8_t *data, uint16_t length)
{
    if (!fw_update_active) {
        LOG_ERROR_RS485("[FW_UPDATE] No update in progress\r\n");
        return false;
    }
    
    // Update activity timestamp
    fw_update_last_activity = xTaskGetTickCount();
    
    // Check for overflow
    if (fw_update_total_bytes + length > fw_update_expected_size) {
        LOG_ERROR_RS485("[FW_UPDATE] Data overflow: %lu + %u > %lu\r\n",
                       fw_update_total_bytes, length, fw_update_expected_size);
        fw_update_abort();
        return false;
    }
    
    // Copy data to buffer
    uint16_t bytes_remaining = length;
    uint16_t data_offset = 0;
    
    while (bytes_remaining > 0) {
        // Calculate how much can fit in current buffer
        uint16_t space_in_buffer = FW_UPDATE_BUFFER_SIZE - fw_update_buffer_index;
        uint16_t bytes_to_copy = (bytes_remaining < space_in_buffer) ? 
                                  bytes_remaining : space_in_buffer;
        
        // Copy to buffer
        memcpy(&fw_update_buffer[fw_update_buffer_index], 
               &data[data_offset], 
               bytes_to_copy);
        
        fw_update_buffer_index += bytes_to_copy;
        data_offset += bytes_to_copy;
        bytes_remaining -= bytes_to_copy;
        
        // If buffer is full, write to flash
        if (fw_update_buffer_index >= FW_UPDATE_BUFFER_SIZE) {
            uint32_t flash_offset = fw_update_total_bytes;
            
            LOG_DEBUG_RS485("[FW_UPDATE] Writing sector at offset 0x%08lX\r\n", flash_offset);
            
            // Disable interrupts for flash write
            uint32_t ints = save_and_disable_interrupts();
            
            // Erase and program flash sector
            flash_range_erase(flash_offset, FW_UPDATE_BUFFER_SIZE);
            flash_range_program(flash_offset, fw_update_buffer, FW_UPDATE_BUFFER_SIZE);
            
            restore_interrupts(ints);
            
            // Update counters
            fw_update_total_bytes += FW_UPDATE_BUFFER_SIZE;
            fw_update_buffer_index = 0;
            
            LOG_DEBUG_RS485("[FW_UPDATE] Progress: %lu / %lu bytes (%.1f%%)\r\n",
                           fw_update_total_bytes, fw_update_expected_size,
                           (float)fw_update_total_bytes * 100.0f / fw_update_expected_size);
        }
    }
    
    return true;
}

/**
 * @brief Finalize firmware update
 * @return true if successful
 */
static bool fw_update_finalize(void)
{
    if (!fw_update_active) {
        LOG_ERROR_RS485("[FW_UPDATE] No update in progress\r\n");
        return false;
    }
    
    // Write remaining data in buffer (if any)
    if (fw_update_buffer_index > 0) {
        // Pad buffer to flash write size (256 bytes minimum)
        uint32_t write_size = ((fw_update_buffer_index + 255) / 256) * 256;
        
        // Zero-fill remaining bytes
        memset(&fw_update_buffer[fw_update_buffer_index], 0xFF, 
               write_size - fw_update_buffer_index);
        
        uint32_t flash_offset = fw_update_total_bytes;
        
        LOG_DEBUG_RS485("[FW_UPDATE] Writing final %lu bytes at offset 0x%08lX\r\n",
                       write_size, flash_offset);
        
        // Disable interrupts for flash write
        uint32_t ints = save_and_disable_interrupts();
        
        // Erase sector if needed
        if (fw_update_buffer_index > (FW_UPDATE_BUFFER_SIZE - write_size)) {
            flash_range_erase(flash_offset, FW_UPDATE_BUFFER_SIZE);
        }
        flash_range_program(flash_offset, fw_update_buffer, write_size);
        
        restore_interrupts(ints);
        
        fw_update_total_bytes += fw_update_buffer_index;
    }
    
    // Verify total size matches
    if (fw_update_total_bytes != fw_update_expected_size) {
        LOG_ERROR_RS485("[FW_UPDATE] Size mismatch: received %lu, expected %lu\r\n",
                       fw_update_total_bytes, fw_update_expected_size);
        fw_update_abort();
        return false;
    }
    
    LOG_DEBUG_RS485("[FW_UPDATE] Complete - %lu bytes written\r\n", fw_update_total_bytes);
    
    fw_update_active = false;
    return true;
}

/**
 * @brief Abort firmware update
 */
static void fw_update_abort(void)
{
    if (fw_update_active) {
        LOG_DEBUG_RS485("[FW_UPDATE] Aborted - %lu bytes received\r\n", fw_update_total_bytes);
    }
    
    fw_update_active = false;
    fw_update_buffer_index = 0;
    fw_update_total_bytes = 0;
    fw_update_expected_size = 0;
}

/**
 * @brief Check for firmware update timeout
 */
static void fw_update_check_timeout(void)
{
    if (!fw_update_active) {
        return;
    }
    
    uint32_t elapsed = (xTaskGetTickCount() - fw_update_last_activity) * portTICK_PERIOD_MS;
    
    if (elapsed > FW_UPDATE_TIMEOUT_MS) {
        LOG_ERROR_RS485("[FW_UPDATE] Timeout - aborting after %lu ms\r\n", elapsed);
        fw_update_abort();
    }
}