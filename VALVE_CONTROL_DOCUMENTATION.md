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
 * @file VALVE_CONTROL_DOCUMENTATION.md
 * @brief Comprehensive documentation for water dispenser valve control system
 * @details Explains the valve control architecture, safety mechanisms, and
 *          integration with MIFARE card system for secure water dispensing
 */

# Water Dispenser Valve Control System

## Overview
The miWota water dispensing system uses **valve control** (not relay control) to manage water flow. The system integrates MIFARE RFID cards with precise flow monitoring and atomic transaction management to ensure secure, corruption-resistant water dispensing.

## Hardware Architecture

### Valve Control Hardware
- **Primary Valve Control**: GPIO 15 (Physical pin 20) - `VALVE_CONTROL_PIN`
- **Alternative Control**: CAT9555 I/O Expander Pin 0 - `RELAY_CONTROL_0_POS`
- **Flow Sensor**: GPIO 22 (Physical pin 29) - YS-S201 flow sensor
- **Card Reader**: I2C_0 Bus - PN532 NFC/RFID reader

### System Components
```
┌─────────────────┐    ┌──────────────┐    ┌─────────────┐
│   MIFARE Card   │◄──►│   PN532      │◄──►│  Pico W     │
│   (Credit Data) │    │   Reader     │    │  Controller │
└─────────────────┘    └──────────────┘    └─────┬───────┘
                                                 │
┌─────────────────┐    ┌──────────────┐         │
│   Flow Sensor   │◄──►│   Valve      │◄────────┘
│   (YS-S201)     │    │   Control    │
└─────────────────┘    └──────────────┘
```

## Software Architecture

### Atomic Transaction System
The system implements a robust atomic transaction mechanism to prevent data corruption:

#### 1. **Transaction States**
```c
typedef enum {
    TRANSACTION_STATE_IDLE = 0x00,
    TRANSACTION_STATE_STARTED = 0x01,
    TRANSACTION_STATE_IN_PROGRESS = 0x02,
    TRANSACTION_STATE_COMMIT_READY = 0x03,
    TRANSACTION_STATE_COMMITTED = 0x04,
    TRANSACTION_STATE_ROLLBACK = 0xFF
} TransactionState_t;
```

#### 2. **Data Protection**
- **Primary + Backup Blocks**: Data written to both primary (Block 5) and backup (Block 6)
- **CRC Checksums**: All data validated with CRC16 checksums
- **Sequence Numbers**: Detect incomplete writes and corruption
- **Transaction Logging**: Audit trail of all operations

### Valve Control Flow

#### Normal Dispensing Operation
```
1. Card Detected → Authenticate & Read Balance
2. User Requests Water → Validate Sufficient Balance
3. Begin Transaction → Write TRANSACTION_STATE_STARTED
4. Open Valve → Start Flow Monitoring
5. Monitor Progress → Update Card Every 500ms
6. Target Reached → Close Valve
7. Commit Transaction → Write TRANSACTION_STATE_COMMITTED
```

#### Emergency Card Removal
```
1. Card Removed Detected (within 50ms)
2. IMMEDIATE Valve Close
3. Stop Flow Monitoring
4. Calculate Actual Volume Dispensed
5. Rollback Transaction State
6. Log Incomplete Transaction
7. Return to Idle State
```

## Safety Mechanisms

### 1. **Continuous Card Monitoring**
- **50ms scanning** during active dispensing
- **Real-time presence verification**
- **Immediate detection** of card removal or swapping

### 2. **Emergency Safety Systems**
- **Valve Close Timeout**: 30-second maximum dispense time
- **Flow Rate Limiting**: Maximum 5 L/min flow rate
- **Volume Limits**: 50mL minimum, 5L maximum per transaction
- **Card Removal Protection**: Immediate valve close + rollback

### 3. **Data Integrity Protection**
- **Backup Block Recovery**: Automatic restoration from backup
- **CRC Validation**: All reads/writes verified
- **Transaction Rollback**: Restore original balance on failure
- **Audit Logging**: Complete transaction history

## API Usage Examples

### Basic Dispensing
```c
// Initialize the system
MIFARE_Dispenser_Init();

// Request 500mL of water
DispenserResult_t result = MIFARE_Dispenser_RequestWater(500);

if (result == DISPENSER_RESULT_OK) {
    // Dispensing started successfully
    // System automatically handles:
    // - Card monitoring
    // - Valve control  
    // - Flow measurement
    // - Transaction updates
    // - Emergency protection
}
```

### Status Monitoring
```c
DispenserStatus_t status;
MIFARE_Dispenser_GetStatus(&status);

printf("Valve: %s\n", status.valve_open ? "OPEN" : "CLOSED");
printf("Dispensed: %u mL\n", status.dispensed_amount_ml);
printf("Flow Rate: %.2f L/min\n", status.current_flow_rate_lpm);
printf("Card Balance: %lu mL\n", status.balance_ml);
```

## Error Handling

### Card Removal During Dispensing
When a card is removed during active dispensing:

1. **Detection**: Card absence detected within 50ms
2. **Emergency Stop**: Valve closes immediately
3. **Flow Calculation**: System calculates actual volume dispensed
4. **Transaction Rollback**: Original balance restored to prevent loss
5. **Audit Log**: Event recorded with timestamp and details
6. **User Feedback**: Display shows "Card Removed - Transaction Cancelled"

### Data Corruption Recovery
If card data becomes corrupted:

1. **Primary Data Check**: CRC validation fails on primary block
2. **Backup Recovery**: System reads backup block (Block 6)
3. **Backup Validation**: CRC check on backup data
4. **Data Restoration**: Valid backup copied to primary block
5. **Recovery Logging**: Recovery attempt logged to audit trail

### Communication Errors
If PN532 communication fails:

1. **Retry Logic**: Up to 3 retry attempts
2. **Error Escalation**: After 3 failures, enter error state
3. **Safe State**: Valve remains closed during errors
4. **Recovery**: Automatic recovery when communication restored

## Configuration Parameters

### Timing Parameters
```c
#define MIFARE_UPDATE_INTERVAL_MS       500     // Card update frequency during dispensing
#define PN532_DISPENSING_SCAN_INTERVAL  50      // Card presence check frequency
#define DISPENSER_SAFETY_TIMEOUT_MS     30000   // Maximum dispensing time
```

### Flow Parameters  
```c
#define DISPENSER_MAX_FLOW_RATE_LPM     5.0f    // Maximum allowed flow rate
#define DISPENSER_MIN_DISPENSE_ML       50      // Minimum dispensing amount
#define DISPENSER_MAX_DISPENSE_ML       5000    // Maximum dispensing amount
```

### Data Integrity
```c
#define MIFARE_MAGIC_BYTES              0x6D575441UL  // "mWTA" system identifier
#define MIFARE_FORMAT_VERSION           0x01          // Data format version
#define MIFARE_MAX_TRANSACTIONS         8             // Transaction log size
```

## Maintenance and Diagnostics

### Debug Logging
The system provides comprehensive logging:
```
MIFARE: Card detected and validated - Ready for dispensing
MIFARE: Card balance: 2500 mL
DISPENSER: Water dispense requested: 500 mL
DISPENSER: Valve OPEN (GPIO 15)
MIFARE: Transaction progress updated: 250 mL dispensed, 2250 mL remaining
DISPENSER: Target amount reached
DISPENSER: Valve CLOSED (GPIO 15)
MIFARE: Transaction committed successfully - 500 mL dispensed
```

### Error Diagnostics
```
PN532: CRITICAL - Card removed during dispensing!
DISPENSER: EMERGENCY VALVE CLOSE - Card removed during dispensing
MIFARE: CRITICAL: Card removed during dispensing - initiating rollback
MIFARE: Emergency rollback: 125 mL was dispensed before card removal
```

## Integration Notes

### Hardware Requirements
- **Pico W Controller**: Main processing unit
- **PN532 NFC Reader**: MIFARE card communication
- **YS-S201 Flow Sensor**: Precise flow measurement
- **Valve Control Circuit**: GPIO 15 or CAT9555 Pin 0
- **Power Supply**: 5V for sensors, 3.3V for controller

### Software Dependencies
- **FreeRTOS**: Real-time task scheduling
- **PN532 Driver**: NFC/RFID communication
- **YS-S201 Driver**: Flow sensor interface
- **CAT9555 Driver**: I/O expander control (if used)

This documentation provides a complete overview of the valve control system with robust safety mechanisms and atomic transaction management for secure water dispensing.