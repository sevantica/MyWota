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
 * @file Customer_Card_Example.c
 * @brief Example implementation showing how to use the customer card initialization
 * @details Provides practical examples for initializing MIFARE cards for new customers
 *          with different scenarios and proper error handling
 */

/*Includes ----------------------------------------------------------*/
#include "Customer_Card_Example.h"
#include "MIFARE_Dispenser_Integration.h"
#include "USB_Logging.h"
#include "FreeRTOS.h"
#include "task.h"

/*Private defines ---------------------------------------------------*/
#define DEFAULT_INITIAL_BALANCE_ML      5000    // 5 liters default balance
#define PREMIUM_INITIAL_BALANCE_ML      10000   // 10 liters for premium customers

/*Function Implementations ------------------------------------------*/

/**
 * @brief Example function showing how to initialize a new customer card
 * @details This example demonstrates the proper usage of the customer card
 *          initialization function with different scenarios
 */
void CustomerCard_InitializationExample(void)
{
    USB_Log_Printf("=== Customer Card Initialization Examples ===\r\n");
    
    // Example 1: Initialize card with auto-generated customer ID
    USB_Log_Printf("\r\nExample 1: Auto-generate customer ID from card serial\r\n");
    DispenserResult_t result = CustomerCard_InitializeAutoID(DEFAULT_INITIAL_BALANCE_ML);
    
    if (result == DISPENSER_RESULT_OK) {
        USB_Log_Printf("✓ Card initialized successfully with auto-generated ID\r\n");
    } else {
        USB_Log_Printf("✗ Card initialization failed: %d\r\n", result);
    }
    
    // Wait a bit before next example
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Example 2: Initialize card with specific customer ID
    USB_Log_Printf("\r\nExample 2: Initialize with specific customer ID\r\n");
    uint64_t specific_customer_id = 1234567890ULL;
    result = CustomerCard_InitializeWithID(PREMIUM_INITIAL_BALANCE_ML, specific_customer_id);
    
    if (result == DISPENSER_RESULT_OK) {
        USB_Log_Printf("✓ Card initialized successfully with ID: %llu\r\n", specific_customer_id);
    } else {
        USB_Log_Printf("✗ Card initialization failed: %d\r\n", result);
    }
    
    // Example 3: Show error handling for different scenarios
    USB_Log_Printf("\r\nExample 3: Error handling scenarios\r\n");
    
    // Try to initialize with invalid balance (0)
    result = CustomerCard_InitializeAutoID(0);
    if (result != DISPENSER_RESULT_OK) {
        USB_Log_Printf("✓ Correctly rejected invalid balance (0 mL)\r\n");
    }
    
    // Try to initialize when no card is present
    USB_Log_Printf("Remove card and press any key to test no-card scenario...\r\n");
    // Note: In a real implementation, you'd wait for user input here
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    result = CustomerCard_InitializeAutoID(DEFAULT_INITIAL_BALANCE_ML);
    if (result == DISPENSER_RESULT_NO_CARD) {
        USB_Log_Printf("✓ Correctly detected no card present\r\n");
    }
    
    USB_Log_Printf("\r\n=== Customer Card Initialization Examples Complete ===\r\n");
}

/**
 * @brief Initialize a card with predefined customer ID
 * @param initial_balance_ml Starting balance in milliliters
 * @param customer_id Specific customer identifier
 * @return DispenserResult_t Operation result
 */
DispenserResult_t CustomerCard_InitializeWithID(uint32_t initial_balance_ml, uint64_t customer_id)
{
    // Validate input parameters
    if (initial_balance_ml == 0) {
        USB_Log_Printf("ERROR: Initial balance cannot be zero\r\n");
        return DISPENSER_RESULT_INVALID_AMOUNT;
    }
    
    if (initial_balance_ml > 50000) { // Max 50 liters
        USB_Log_Printf("ERROR: Initial balance too large (max 50L)\r\n");
        return DISPENSER_RESULT_INVALID_AMOUNT;
    }
    
    if (customer_id == 0) {
        USB_Log_Printf("ERROR: Customer ID cannot be zero when specified\r\n");
        return DISPENSER_RESULT_ERROR;
    }
    
    USB_Log_Printf("Initializing card for Customer ID: %llu with %u mL balance\r\n", 
                   customer_id, initial_balance_ml);
    
    // Call the dispenser integration function
    DispenserResult_t result = MIFARE_Dispenser_InitializeNewCustomer(initial_balance_ml, customer_id);
    
    // Log the result
    switch (result) {
        case DISPENSER_RESULT_OK:
            USB_Log_Printf("SUCCESS: Card initialized successfully\r\n");
            USB_Log_Printf("Customer can now use the card for water dispensing\r\n");
            break;
            
        case DISPENSER_RESULT_NO_CARD:
            USB_Log_Printf("ERROR: No card detected. Please place card on reader\r\n");
            break;
            
        case DISPENSER_RESULT_BUSY:
            USB_Log_Printf("ERROR: System busy. Please try again later\r\n");
            break;
            
        case DISPENSER_RESULT_CARD_ERROR:
            USB_Log_Printf("ERROR: Card communication or authentication failed\r\n");
            break;
            
        default:
            USB_Log_Printf("ERROR: Initialization failed with error code: %d\r\n", result);
            break;
    }
    
    return result;
}

/**
 * @brief Initialize a card with auto-generated customer ID from card serial
 * @param initial_balance_ml Starting balance in milliliters  
 * @return DispenserResult_t Operation result
 */
DispenserResult_t CustomerCard_InitializeAutoID(uint32_t initial_balance_ml)
{
    // Validate input parameters
    if (initial_balance_ml == 0) {
        USB_Log_Printf("ERROR: Initial balance cannot be zero\r\n");
        return DISPENSER_RESULT_INVALID_AMOUNT;
    }
    
    if (initial_balance_ml > 50000) { // Max 50 liters
        USB_Log_Printf("ERROR: Initial balance too large (max 50L)\r\n");
        return DISPENSER_RESULT_INVALID_AMOUNT;
    }
    
    USB_Log_Printf("Initializing card with auto-generated ID and %u mL balance\r\n", 
                   initial_balance_ml);
    
    // Call the dispenser integration function with customer_id = 0 for auto-generation
    DispenserResult_t result = MIFARE_Dispenser_InitializeNewCustomer(initial_balance_ml, 0);
    
    // Log the result
    switch (result) {
        case DISPENSER_RESULT_OK:
            USB_Log_Printf("SUCCESS: Card initialized successfully with auto-generated ID\r\n");
            USB_Log_Printf("Customer can now use the card for water dispensing\r\n");
            break;
            
        case DISPENSER_RESULT_NO_CARD:
            USB_Log_Printf("ERROR: No card detected. Please place card on reader\r\n");
            break;
            
        case DISPENSER_RESULT_BUSY:
            USB_Log_Printf("ERROR: System busy. Please try again later\r\n");
            break;
            
        case DISPENSER_RESULT_CARD_ERROR:
            USB_Log_Printf("ERROR: Card communication or authentication failed\r\n");
            break;
            
        default:
            USB_Log_Printf("ERROR: Initialization failed with error code: %d\r\n", result);
            break;
    }
    
    return result;
}

/*Example Usage in Main Application ---------------------------------*/

/*
 * To use the customer card initialization in your main application:
 * 
 * 1. Include the header:
 *    #include "Customer_Card_Example.h"
 * 
 * 2. Initialize a card with auto-generated ID:
 *    DispenserResult_t result = CustomerCard_InitializeAutoID(5000); // 5L balance
 * 
 * 3. Initialize a card with specific customer ID:
 *    DispenserResult_t result = CustomerCard_InitializeWithID(10000, 1234567890ULL);
 * 
 * 4. Check the result and handle accordingly:
 *    if (result == DISPENSER_RESULT_OK) {
 *        // Card ready for use
 *    } else {
 *        // Handle error based on result code
 *    }
 * 
 * 5. Run the complete example demo:
 *    CustomerCard_InitializationExample();
 */