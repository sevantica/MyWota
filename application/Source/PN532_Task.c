/*
 * @attention
 * Copyright (c) Sevantica 2025.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

/**
 * @file PN532_Task.c
 * @brief PN532 NFC/RFID Task Implementation
 * @details Implements FreeRTOS task for PN532 card detection and processing
 */

/* Includes ------------------------------------------------------------------*/
#include "PN532_Task.h"
#include "PN532_Driver.h"
#include "MIFARE_Transaction_Manager.h"
#include "USB_Logging.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "System.h"
#include "Task_Heartbeat.h"
#include "task_stack_config.h"
#include <string.h>
#include <stdio.h>

/* Private defines -----------------------------------------------------------*/
#define PN532_SCAN_INTERVAL_MS              (100U)  /* 100ms scan interval as requested */
#define PN532_DISPENSING_SCAN_INTERVAL_MS   (50U)   /* Faster scanning during dispensing */
#define PN532_INIT_RETRY_COUNT              (3U)    /* Number of initialization retries */
#define PN532_INIT_RETRY_DELAY_MS           (500U)  /* Delay between init retries */
#define PN532_CARD_DEBOUNCE_COUNT           (3U)    /* Consecutive detections needed */
#define PN532_CARD_TIMEOUT_MS               (5000U) /* Card timeout for absence detection */
#define PN532_TRANSACTION_TIMEOUT_MS        (30000U)/* Maximum transaction duration */

/* Private typedefs ----------------------------------------------------------*/
typedef enum {
    PN532_TASK_STATE_INIT,
    PN532_TASK_STATE_IDLE,
    PN532_TASK_STATE_CARD_DETECTED,
    PN532_TASK_STATE_CARD_PROCESSING,
    PN532_TASK_STATE_ERROR
} PN532_TaskState_t;

/* Private variables ---------------------------------------------------------*/
static TaskHandle_t PN532_Driver_TaskHandle = NULL;
static PN532_TaskState_t current_state = PN532_TASK_STATE_INIT;
static PN532_CardInfo_t last_detected_card;
static uint8_t card_detection_count = 0;
static uint8_t initialization_attempts = 0;
static bool mifare_manager_initialized = false;
static uint32_t last_card_presence_check = 0;
static uint32_t transaction_start_time = 0;

/* Private function prototypes -----------------------------------------------*/
static void PN532_Driver_Task(void* argument);
static PN532_Status_t PN532_InitializeWithRetry(void);
static void PN532_ProcessNewCard(PN532_CardInfo_t *card_info);
static uint8_t PN532_CompareCards(PN532_CardInfo_t *card1, PN532_CardInfo_t *card2);
static const char* PN532_GetCardTypeName(PN532_CardType_t card_type);
static void PN532_PrintCardInfo(PN532_CardInfo_t *card_info);
static void PN532_HandleDispensingMode(void);
static uint32_t PN532_GetScanInterval(void);

/* Task interface functions -------------------------------------------------*/

/**
 * @brief Start the PN532 driver FreeRTOS task
 * This function creates and starts the PN532_Driver_Task
 */
void Task_Start_PN532_Driver_Task(void)
{
    BaseType_t task_result = xTaskCreate(
        PN532_Driver_Task,                    // Task function
        "PN532_Driver Task",                  // Task name
        PN532_DRIVER_TASK_STACK_WORDS,        // Stack size in words
        NULL,                                 // Task parameters
        PN532_TASK_PRIORITY ,                 // Task priority
        &PN532_Driver_TaskHandle              // Task handle
    );
    
    if (task_result != pdPASS) {
        // Task creation failed - log error
        USB_Log_Printf("PN532: Task creation failed!\r\n");
        PN532_Driver_TaskHandle = NULL;
    } else {
        USB_Log_Printf("PN532: Task created successfully\r\n");
    }
}

/**
 * @brief Get the handle for the PN532 driver FreeRTOS task
 * @return TaskHandle_t The task handle for the PN532_Driver_Task
 */
TaskHandle_t task_get_handle_PN532_Driver_Task(void)
{
    return PN532_Driver_TaskHandle;
}

/* Private function implementations ------------------------------------------*/

/**
 * @brief Main PN532 driver task implementing state machine for card detection
 * 
 * State Machine Flow:
 * INIT -> Initialize PN532 hardware and verify communication
 * IDLE -> Scan for cards every 100ms, handle card detection
 * CARD_DETECTED -> Validate card presence and process card information
 * CARD_PROCESSING -> Handle card operations and monitor for card removal
 * ERROR -> Handle error conditions and attempt recovery
 * 
 * @param argument Unused task parameter
 */
static void PN532_Driver_Task(void* argument)
{
    PN532_Status_t status;
    PN532_CardInfo_t current_card;
    
    // Wait for task start notification from system
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    
    // Initialize variables
    memset(&last_detected_card, 0, sizeof(PN532_CardInfo_t));
    memset(&current_card, 0, sizeof(PN532_CardInfo_t));
    
    // Main task loop
    for (;;) {
        TASK_HEARTBEAT_EVERY_SECOND("PN532");
        
        switch (current_state) {
            
            case PN532_TASK_STATE_INIT:
                /*
                 * INITIALIZATION STATE: Setup PN532 hardware and verify communication
                 * Entry: System startup or after error recovery
                 * Exit: Success -> IDLE, Failure -> ERROR (with retry logic)
                 */
                status = PN532_InitializeWithRetry();
                if (status == PN532_STATUS_OK) {
                    USB_Log_Printf("PN532: Initialization successful\r\n");
                    
                    // Initialize MIFARE transaction manager
                    if (!mifare_manager_initialized) {
                        MIFARE_Result_t mifare_result = MIFARE_TransactionManager_Init();
                        if (mifare_result == MIFARE_RESULT_OK) {
                            mifare_manager_initialized = true;
                            USB_Log_Printf("PN532: MIFARE Transaction Manager initialized\r\n");
                        } else {
                            USB_Log_Printf("PN532: Failed to initialize MIFARE Transaction Manager\r\n");
                        }
                    }
                    
                    current_state = PN532_TASK_STATE_IDLE;
                    card_detection_count = 0;
                } else {
                    USB_Log_Printf("PN532: Initialization failed after %d attempts\r\n", PN532_INIT_RETRY_COUNT);
                    current_state = PN532_TASK_STATE_ERROR;
                }
                break;
                
            case PN532_TASK_STATE_IDLE:
                /*
                 * IDLE STATE: Continuous card scanning and detection
                 * Entry: After successful initialization or card removal
                 * Exit: Card detected -> CARD_DETECTED, Error -> ERROR
                 */
                status = PN532_DetectCard(&current_card);
                
                if (status == PN532_STATUS_CARD_DETECTED) {
                    // Check if this is the same card as before
                    if (PN532_CompareCards(&current_card, &last_detected_card)) {
                        card_detection_count++;
                        if (card_detection_count >= PN532_CARD_DEBOUNCE_COUNT) {
                            USB_Log_Printf("PN532: Card detected and validated\r\n");
                            PN532_PrintCardInfo(&current_card);
                            current_state = PN532_TASK_STATE_CARD_DETECTED;
                        }
                    } else {
                        // New different card detected
                        memcpy(&last_detected_card, &current_card, sizeof(PN532_CardInfo_t));
                        card_detection_count = 1;
                    }
                } else if (status == PN532_STATUS_NO_CARD) {
                    // No card present - reset detection counter
                    if (card_detection_count > 0) {
                        USB_Log_Printf("PN532: Card removed\r\n");
                        memset(&last_detected_card, 0, sizeof(PN532_CardInfo_t));
                    }
                    card_detection_count = 0;
                } else {
                    // Communication error
                    USB_Log_Printf("PN532: Communication error in IDLE state\r\n");
                    current_state = PN532_TASK_STATE_ERROR;
                }
                break;
                
            case PN532_TASK_STATE_CARD_DETECTED:
                /*
                 * CARD_DETECTED STATE: Process newly detected card
                 * Entry: After successful card detection and validation
                 * Exit: Always -> CARD_PROCESSING
                 */
                PN532_ProcessNewCard(&last_detected_card);
                current_state = PN532_TASK_STATE_CARD_PROCESSING;
                break;
                
            case PN532_TASK_STATE_CARD_PROCESSING:
                /*
                 * CARD_PROCESSING STATE: Monitor card presence and handle operations
                 * Entry: After card detection and initial processing
                 * Exit: Card removed -> IDLE, Error -> ERROR
                 */
                
                // Handle dispensing mode with continuous monitoring
                if (mifare_manager_initialized) {
                    MIFARE_DispenseState_t dispense_state = MIFARE_GetDispenseState();
                    if (dispense_state == DISPENSE_STATE_DISPENSING || 
                        dispense_state == DISPENSE_STATE_UPDATING_CARD) {
                        PN532_HandleDispensingMode();
                        break;
                    }
                }
                
                status = PN532_DetectCard(&current_card);
                
                if (status == PN532_STATUS_CARD_DETECTED) {
                    // Verify it's still the same card
                    if (!PN532_CompareCards(&current_card, &last_detected_card)) {
                        // Different card detected
                        USB_Log_Printf("PN532: Card changed during processing\r\n");
                        
                        // Notify transaction manager of card removal
                        if (mifare_manager_initialized) {
                            MIFARE_ProcessCardRemoved();
                        }
                        
                        // Process new card
                        memcpy(&last_detected_card, &current_card, sizeof(PN532_CardInfo_t));
                        PN532_ProcessNewCard(&last_detected_card);
                    } else {
                        // Same card - perform periodic monitoring
                        if (mifare_manager_initialized) {
                            MIFARE_MonitorCardPresence();
                        }
                    }
                } else if (status == PN532_STATUS_NO_CARD) {
                    // Card removed
                    USB_Log_Printf("PN532: Card removed\r\n");
                    
                    // Notify transaction manager
                    if (mifare_manager_initialized) {
                        MIFARE_ProcessCardRemoved();
                    }
                    
                    memset(&last_detected_card, 0, sizeof(PN532_CardInfo_t));
                    current_state = PN532_TASK_STATE_IDLE;
                } else {
                    // Communication error
                    USB_Log_Printf("PN532: Communication error in CARD_PROCESSING state\r\n");
                    current_state = PN532_TASK_STATE_ERROR;
                }
                break;
                
            case PN532_TASK_STATE_ERROR:
                /*
                 * ERROR STATE: Handle error conditions and attempt recovery
                 * Entry: After communication failures or initialization errors
                 * Exit: Recovery attempt -> INIT, Fatal error -> stays in ERROR
                 */
                USB_Log_Printf("PN532: In ERROR state, attempting recovery...\r\n");
                vTaskDelay(pdMS_TO_TICKS(1000)); // Wait before retry
                
                initialization_attempts = 0; // Reset retry counter
                current_state = PN532_TASK_STATE_INIT;
                break;
                
            default:
                // Invalid state - return to initialization
                USB_Log_Printf("PN532: Invalid state, returning to INIT\r\n");
                current_state = PN532_TASK_STATE_INIT;
                break;
        }
        
        // Dynamic scan interval based on state
        uint32_t scan_interval = PN532_GetScanInterval();
        vTaskDelay(pdMS_TO_TICKS(scan_interval));
    }
}

/**
 * @brief Initialize PN532 with retry mechanism
 * @return PN532_Status_t Status of initialization
 */
static PN532_Status_t PN532_InitializeWithRetry(void)
{
    PN532_Status_t status;
    PN532_FirmwareInfo_t firmware_info;
    
    for (initialization_attempts = 0; initialization_attempts < PN532_INIT_RETRY_COUNT; initialization_attempts++) {
        USB_Log_Printf("PN532: Initialization attempt %d/%d\r\n", initialization_attempts + 1, PN532_INIT_RETRY_COUNT);
        
        // Initialize PN532
        status = PN532_Init();
        if (status != PN532_STATUS_OK) {
            USB_Log_Printf("PN532: Init failed on attempt %d\r\n", initialization_attempts + 1);
            vTaskDelay(pdMS_TO_TICKS(PN532_INIT_RETRY_DELAY_MS));
            continue;
        }
        
        // Verify communication by reading firmware version
        status = PN532_GetFirmwareVersion(&firmware_info);
        if (status == PN532_STATUS_OK) {
         USB_Log_Printf("PN532: Firmware v%d.%d (IC: 0x%02X, Support: 0x%02X)\r\n",
             firmware_info.Ver, firmware_info.Rev,
             firmware_info.IC, firmware_info.Support);
            return PN532_STATUS_OK;
        }
        
        USB_Log_Printf("PN532: Firmware read failed on attempt %d\r\n", initialization_attempts + 1);
        vTaskDelay(pdMS_TO_TICKS(PN532_INIT_RETRY_DELAY_MS));
    }
    
    return PN532_STATUS_ERROR;
}

/**
 * @brief Process newly detected card
 * @param card_info Pointer to card information structure
 */
static void PN532_ProcessNewCard(PN532_CardInfo_t *card_info)
{
    if (card_info == NULL) {
        return;
    }
    
    USB_Log_Printf("PN532: Processing new card\r\n");
    
    // For MIFARE Classic cards, integrate with transaction manager
    if (card_info->card_type == PN532_CARD_MIFARE_CLASSIC_1K || 
        card_info->card_type == PN532_CARD_MIFARE_CLASSIC_4K) {
        
        if (mifare_manager_initialized) {
            // Let the transaction manager handle the card
            MIFARE_Result_t result = MIFARE_ProcessCardDetected(card_info);
            
            if (result == MIFARE_RESULT_OK) {
                USB_Log_Printf("PN532: Card processed successfully by transaction manager\r\n");
                uint32_t balance = MIFARE_GetBalanceML();
                USB_Log_Printf("PN532: Card balance: %lu mL\r\n", balance);
            } else {
                USB_Log_Printf("PN532: Transaction manager failed to process card: %s\r\n", 
                               MIFARE_GetResultString(result));
            }
        } else {
            // Fallback to basic authentication for debugging
            uint8_t default_key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            PN532_Status_t auth_status = PN532_MifareAuthenticate(4, card_info->uid, card_info->uid_length, default_key);
            
            if (auth_status == PN532_STATUS_OK) {
                USB_Log_Printf("PN532: MIFARE authentication successful\r\n");
                
                // Example: Read block 4
                uint8_t block_data[16];
                PN532_Status_t read_status = PN532_MifareReadBlock(4, block_data);
                if (read_status == PN532_STATUS_OK) {
                    USB_Log_Printf("PN532: Block 4 data: ");
                    for (int i = 0; i < 16; i++) {
                        USB_Log_Printf("%02X ", block_data[i]);
                    }
                    USB_Log_Printf("\r\n");
                }
            } else {
                USB_Log_Printf("PN532: MIFARE authentication failed\r\n");
            }
        }
    }
}

/**
 * @brief Compare two cards to check if they are the same using robust verification
 * @param card1 Pointer to first card information
 * @param card2 Pointer to second card information
 * @return uint8_t 1 if cards are the same, 0 otherwise
 */
static uint8_t PN532_CompareCards(PN532_CardInfo_t *card1, PN532_CardInfo_t *card2)
{
    if (card1 == NULL || card2 == NULL) {
        return 0;
    }
    
    // First level: Basic card structure comparison
    // Compare UID length first
    if (card1->uid_length != card2->uid_length) {
        USB_Log_Printf("PN532: Card comparison failed - UID length mismatch (%d vs %d)\r\n", 
                       card1->uid_length, card2->uid_length);
        return 0;
    }
    
    // Compare UID bytes
    if (memcmp(card1->uid, card2->uid, card1->uid_length) != 0) {
        USB_Log_Printf("PN532: Card comparison failed - UID mismatch\r\n");
        return 0;
    }
    
    // Compare card type
    if (card1->card_type != card2->card_type) {
        USB_Log_Printf("PN532: Card comparison failed - card type mismatch\r\n");
        return 0;
    }
    
    // Second level: Enhanced verification using card-stored unique data
    // For MIFARE cards, read and compare stored card serial and transaction data
    if ((card1->card_type == PN532_CARD_MIFARE_CLASSIC_1K || 
         card1->card_type == PN532_CARD_MIFARE_CLASSIC_4K) && 
        mifare_manager_initialized) {
        
        // Authenticate and read unique card data for verification
        uint8_t auth_key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // Default key
        uint8_t header_block[16];
        
        // Authenticate header block
        PN532_Status_t auth_status = PN532_MifareAuthenticate(MIFARE_BLOCK_HEADER, 
                                                              card1->uid, 
                                                              card1->uid_length, 
                                                              auth_key);
        
        if (auth_status == PN532_STATUS_OK) {
            // Read header block containing unique card serial and magic bytes
            PN532_Status_t read_status = PN532_MifareReadBlock(MIFARE_BLOCK_HEADER, header_block);
            if (read_status == PN532_STATUS_OK) {
                // Verify magic bytes (first 4 bytes should be "mWTA")
                uint32_t magic_bytes = *(uint32_t*)header_block;
                if (magic_bytes != MIFARE_MAGIC_BYTES) {
                    USB_Log_Printf("PN532: Invalid magic bytes in card header: 0x%08lX\r\n", magic_bytes);
                    return 0; // Not a valid miWota card
                }
                
                // Extract card serial number (bytes 8-15)
                uint64_t card_serial = *(uint64_t*)(header_block + 8);
                
                // Read user data block for additional verification
                uint8_t user_data_block[16];
                read_status = PN532_MifareReadBlock(MIFARE_BLOCK_USER_PRIMARY, user_data_block);
                if (read_status == PN532_STATUS_OK) {
                    // Extract transaction counter for enhanced verification
                    uint16_t transaction_counter = *(uint16_t*)(user_data_block + 8);
                    
                    // Calculate verification hash
                    uint32_t verification_hash = (uint32_t)(card_serial ^ ((uint64_t)transaction_counter << 16));
                    
                    // Store verification data for comparison with previous reads
                    static uint64_t last_verified_serial = 0;
                    static uint32_t last_verification_hash = 0;
                    static bool first_verification = true;
                    
                    if (first_verification) {
                        // First verification - store values
                        last_verified_serial = card_serial;
                        last_verification_hash = verification_hash;
                        first_verification = false;
                        USB_Log_Printf("PN532: Card verified - Serial: 0x%08lX%08lX, TxCount: %u\r\n",
                                       (uint32_t)(card_serial >> 32), (uint32_t)card_serial, transaction_counter);
                        return 1;
                    } else {
                        // Compare with stored values
                        if (last_verified_serial == card_serial) {
                            // Same card serial - check transaction counter reasonableness
                            uint32_t hash_diff = (verification_hash > last_verification_hash) ? 
                                                 (verification_hash - last_verification_hash) : 
                                                 (last_verification_hash - verification_hash);
                            
                            if (hash_diff < 0x10000) { // Allow reasonable transaction count changes
                                last_verification_hash = verification_hash;
                                return 1; // Same card
                            } else {
                                USB_Log_Printf("PN532: Suspicious transaction change - possible cloning attempt\r\n");
                                return 0;
                            }
                        } else {
                            USB_Log_Printf("PN532: Different card serial detected\r\n");
                            // Update for new card
                            last_verified_serial = card_serial;
                            last_verification_hash = verification_hash;
                            return 0; // Different card
                        }
                    }
                } else {
                    USB_Log_Printf("PN532: Enhanced verification using header data only\r\n");
                    // Fall back to basic serial comparison
                    static uint64_t stored_serial = 0;
                    static bool serial_init = true;
                    
                    if (serial_init) {
                        stored_serial = card_serial;
                        serial_init = false;
                        return 1;
                    }
                    return (stored_serial == card_serial) ? 1 : 0;
                }
            } else {
                USB_Log_Printf("PN532: Failed to read header block for verification\r\n");
            }
        } else {
            USB_Log_Printf("PN532: Authentication failed for enhanced verification\r\n");
        }
    }
    
    return 1; // Cards are the same (fallback to basic comparison)
}

/**
 * @brief Get string name for card type
 * @param card_type Card type enumeration
 * @return const char* String representation of card type
 */
static const char* PN532_GetCardTypeName(PN532_CardType_t card_type)
{
    switch (card_type) {
        case PN532_CARD_MIFARE_CLASSIC_1K:   return "MIFARE Classic 1K";
        case PN532_CARD_MIFARE_CLASSIC_4K:   return "MIFARE Classic 4K";
        case PN532_CARD_MIFARE_ULTRALIGHT:   return "MIFARE Ultralight";
        case PN532_CARD_MIFARE_DESFIRE:      return "MIFARE DESFire";
        case PN532_CARD_UNKNOWN:             return "Unknown";
        case PN532_CARD_NONE:
        default:                             return "None";
    }
}

/**
 * @brief Print card information to console
 * @param card_info Pointer to card information structure
 */
static void PN532_PrintCardInfo(PN532_CardInfo_t *card_info)
{
    if (card_info == NULL) {
        return;
    }
    
    USB_Log_Printf("PN532: Card Type: %s\r\n", PN532_GetCardTypeName(card_info->card_type));
    USB_Log_Printf("PN532: SAK: 0x%02X\r\n", card_info->sak);
    USB_Log_Printf("PN532: ATQA: 0x%02X%02X\r\n", card_info->atqa[1], card_info->atqa[0]);
    USB_Log_Printf("PN532: UID (%d bytes): ", card_info->uid_length);
    
    for (uint8_t i = 0; i < card_info->uid_length; i++) {
        USB_Log_Printf("%02X", card_info->uid[i]);
        if (i < card_info->uid_length - 1) {
            USB_Log_Printf(":");
        }
    }
    USB_Log_Printf("\r\n");
}

/**
 * @brief Handle dispensing mode with continuous card monitoring
 */
static void PN532_HandleDispensingMode(void)
{
    PN532_Status_t status;
    PN532_CardInfo_t current_card;
    uint32_t current_time = (uint32_t)xTaskGetTickCount();
    
    // Perform more frequent card presence checks during dispensing
    if ((current_time - last_card_presence_check) >= pdMS_TO_TICKS(PN532_DISPENSING_SCAN_INTERVAL_MS)) {
        status = PN532_DetectCard(&current_card);
        last_card_presence_check = current_time;
        
        if (status == PN532_STATUS_CARD_DETECTED) {
            // Verify it's still the same card
            if (PN532_CompareCards(&current_card, &last_detected_card)) {
                // Same card present - perform periodic update
                MIFARE_PerformPeriodicUpdate();
            } else {
                // Different card detected during dispensing
                USB_Log_Printf("PN532: CRITICAL - Different card detected during dispensing!\r\n");
                MIFARE_ProcessCardRemoved();
                memcpy(&last_detected_card, &current_card, sizeof(PN532_CardInfo_t));
                MIFARE_ProcessCardDetected(&last_detected_card);
            }
        } else if (status == PN532_STATUS_NO_CARD) {
            // Card removed during dispensing - critical event
            USB_Log_Printf("PN532: CRITICAL - Card removed during dispensing!\r\n");
            MIFARE_ProcessCardRemoved();
            memset(&last_detected_card, 0, sizeof(PN532_CardInfo_t));
            current_state = PN532_TASK_STATE_IDLE;
        } else {
            // Communication error during dispensing
            USB_Log_Printf("PN532: Communication error during dispensing\r\n");
            // Don't transition to error immediately - try a few more times
            static uint8_t dispense_error_count = 0;
            dispense_error_count++;
            if (dispense_error_count >= 3) {
                current_state = PN532_TASK_STATE_ERROR;
                dispense_error_count = 0;
            }
        }
    }
    
    // Check for transaction timeout
    if (transaction_start_time > 0) {
        if ((current_time - transaction_start_time) >= pdMS_TO_TICKS(PN532_TRANSACTION_TIMEOUT_MS)) {
            USB_Log_Printf("PN532: Transaction timeout - forcing rollback\r\n");
            MIFARE_ProcessCardRemoved();
            transaction_start_time = 0;
        }
    }
}

/**
 * @brief Get appropriate scan interval based on current state
 * @return uint32_t Scan interval in milliseconds
 */
static uint32_t PN532_GetScanInterval(void)
{
    if (mifare_manager_initialized) {
        MIFARE_DispenseState_t dispense_state = MIFARE_GetDispenseState();
        
        switch (dispense_state) {
            case DISPENSE_STATE_DISPENSING:
            case DISPENSE_STATE_UPDATING_CARD:
                return PN532_DISPENSING_SCAN_INTERVAL_MS;
                
            case DISPENSE_STATE_READY_TO_DISPENSE:
            case DISPENSE_STATE_CARD_DETECTED:
                return PN532_SCAN_INTERVAL_MS / 2;  // 50ms for active states
                
            default:
                return PN532_SCAN_INTERVAL_MS;      // 100ms for idle states
        }
    }
    
    return PN532_SCAN_INTERVAL_MS;
}
