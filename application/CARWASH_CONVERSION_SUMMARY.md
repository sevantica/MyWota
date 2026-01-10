# Car Wash Token System Conversion Summary

## Overview
The system has been converted from a liquid dispensing system to a car wash token-based system. This document outlines all structural changes made to the MIFARE transaction management system.

---

## Key Changes

### System Concept
- **Before**: Liquid dispensing system with balance tracked in milliliters (mL)
- **After**: Car wash token system where each token = 1 wash (20-minute session)
- **Card Interaction**: Card only needs to be scanned once at start (no need to remain in field during wash)

---

## Header File Changes

### 1. MIFARE_Transaction_Manager.h

#### Version Update
- **Format Version**: Changed from `0x01` to `0x02` (indicates car wash token system)
- **Magic Bytes**: Kept same (`0x6D575441UL` - "mWTA")

#### Block Layout Changes
```
Block 4:  Header (unchanged)
Block 5:  User Primary → Now stores token_count instead of balance_ml
Block 6:  User Backup (structure updated)
Block 8:  Usage Data → Now stores lifetime wash statistics
Block 9:  Transaction Log (updated for wash transactions)
Block 10: Transaction Log (continued)
Block 12: Recovery Info (unchanged)
Block 13: Fast Balance → Token Cache Primary
Block 14: Fast Balance Backup → Token Cache Backup
Block 16: Account Data (unchanged structure)
Block 17: NEW - Wash Timer Data (tracks active wash session)
```

#### Structure Changes

**MIFARE_UserData_t** (Block 5 & 6)
```c
// OLD:
uint32_t balance_ml;           // Balance in milliliters
uint32_t last_topup_amount_ml; // Last topup in milliliters

// NEW:
uint32_t token_count;          // Current number of wash tokens
uint32_t last_topup_tokens;    // Last topup in tokens
```

**MIFARE_UsageData_t** (Block 8)
```c
// OLD:
uint32_t total_purchased_ml;   // Total water purchased (lifetime)
uint32_t total_dispensed_ml;   // Total water dispensed (lifetime)

// NEW:
uint32_t total_tokens_purchased; // Total tokens purchased (lifetime)
uint32_t total_washes_completed; // Total car washes completed (lifetime)
uint32_t total_wash_time_minutes; // Total wash time used (lifetime)
```

**MIFARE_TransactionRecord_t** (Transaction Log)
```c
// OLD:
uint16_t amount_ml;           // Amount in milliliters
uint8_t transaction_type;     // 1=Purchase, 2=Dispense, 3=Refund
uint8_t dispenser_id;         // Which dispenser was used

// NEW:
uint16_t wash_duration_min;   // Wash duration in minutes
uint8_t transaction_type;     // 1=Token Purchase, 2=Wash Started, 3=Wash Completed, 4=Refund
uint8_t wash_bay_id;          // Which wash bay was used
```

**MIFARE_FastBalance_t → MIFARE_TokenCache_t** (Blocks 13 & 14)
```c
// OLD:
typedef struct {
    uint32_t balance_ml;       // Current balance
    // ... other fields
} MIFARE_FastBalance_t;

// NEW:
typedef struct {
    uint32_t token_count;      // Current token count
    // ... other fields (structure unchanged)
} MIFARE_TokenCache_t;
```

**NEW: MIFARE_WashTimerData_t** (Block 17)
```c
typedef struct __attribute__((packed)) {
    uint32_t wash_start_time;      // Unix timestamp when wash started (0 = no active wash)
    uint16_t wash_duration_seconds; // Configured wash duration (default 1200 = 20 minutes)
    uint8_t wash_active;           // 1 if wash in progress, 0 if idle
    uint8_t reserved1;             // Reserved
    uint32_t tokens_used_this_session; // Tokens consumed in current session
    uint32_t reserved2;            // Reserved
    // Total: 16 bytes
} MIFARE_WashTimerData_t;
```

#### Enum Changes

**Transaction States**
```c
// Changed:
#define TRANSACTION_STATE_IN_PROGRESS   0x02
// To:
#define TRANSACTION_STATE_WASHING       0x02
```

**MIFARE_DispenseState_t → MIFARE_WashState_t**
```c
// OLD:
typedef enum {
    DISPENSE_STATE_IDLE,
    DISPENSE_STATE_CARD_DETECTED,
    DISPENSE_STATE_AUTHENTICATING,
    DISPENSE_STATE_READING_DATA,
    DISPENSE_STATE_VALIDATING,
    DISPENSE_STATE_READY_TO_DISPENSE,
    DISPENSE_STATE_DISPENSING,
    DISPENSE_STATE_UPDATING_CARD,
    DISPENSE_STATE_FINALIZING,
    DISPENSE_STATE_ERROR,
    DISPENSE_STATE_CARD_REMOVED,
    DISPENSE_STATE_CARD_REMOVED_DURING_DISPENSING
} MIFARE_DispenseState_t;

// NEW:
typedef enum {
    WASH_STATE_IDLE,
    WASH_STATE_CARD_DETECTED,
    WASH_STATE_AUTHENTICATING,
    WASH_STATE_READING_DATA,
    WASH_STATE_VALIDATING,
    WASH_STATE_READY_TO_WASH,
    WASH_STATE_WASHING,
    WASH_STATE_UPDATING_CARD,
    WASH_STATE_FINALIZING,
    WASH_STATE_ERROR,
    WASH_STATE_CARD_REMOVED,
    WASH_STATE_TIMER_EXPIRED
} MIFARE_WashState_t;
```

**MIFARE_TransactionManager_t**
```c
// OLD fields:
MIFARE_DispenseState_t dispense_state;
uint32_t dispense_start_time;
uint32_t total_dispensed_this_session;
uint32_t last_fast_balance_update_time;
MIFARE_FastBalance_t fast_balance_primary;
MIFARE_FastBalance_t fast_balance_backup;

// NEW fields:
MIFARE_WashState_t wash_state;
uint32_t wash_start_time;
uint32_t wash_timer_remaining_seconds;
uint32_t last_token_cache_update_time;
MIFARE_TokenCache_t token_cache_primary;
MIFARE_TokenCache_t token_cache_backup;
MIFARE_WashTimerData_t wash_timer;  // Added to card_data
```

#### Function Prototype Changes

**Card Initialization**
```c
// OLD:
MIFARE_Result_t MIFARE_InitializeNewCustomerCard(uint32_t initial_balance_ml, uint64_t customer_id);
MIFARE_Result_t MIFARE_DetectAndAutoInitializeCard(const PN532_CardInfo_t *card_info, uint32_t default_balance_ml);

// NEW:
MIFARE_Result_t MIFARE_InitializeNewCustomerCard(uint32_t initial_tokens, uint64_t customer_id);
MIFARE_Result_t MIFARE_DetectAndAutoInitializeCard(const PN532_CardInfo_t *card_info, uint32_t default_tokens);
```

**Balance/Token Management**
```c
// OLD:
MIFARE_Result_t MIFARE_TopupCardBalance(uint32_t topup_amount_ml);

// NEW:
MIFARE_Result_t MIFARE_TopupCardTokens(uint32_t topup_tokens);
```

**Transaction Operations**
```c
// OLD:
MIFARE_Result_t MIFARE_BeginTransaction(uint32_t amount_ml);
MIFARE_Result_t MIFARE_UpdateTransactionProgress(uint32_t dispensed_ml, float flow_rate_lpm);
MIFARE_Result_t MIFARE_HandleCardRemovalDuringDispense(void);

// NEW:
MIFARE_Result_t MIFARE_BeginWashTransaction(void);  // No amount param - always deducts 1 token
MIFARE_Result_t MIFARE_UpdateWashProgress(uint32_t elapsed_seconds);
MIFARE_Result_t MIFARE_HandleWashTimerExpired(void);
```

**State Management**
```c
// OLD:
MIFARE_DispenseState_t MIFARE_GetDispenseState(void);
const char* MIFARE_GetStateString(MIFARE_DispenseState_t state);

// NEW:
MIFARE_WashState_t MIFARE_GetWashState(void);
const char* MIFARE_GetWashStateString(MIFARE_WashState_t state);
```

**UI Getter Functions**
```c
// OLD:
uint32_t MIFARE_GetBalanceML(void);
uint32_t MIFARE_GetLastTopupAmountML(void);
uint32_t MIFARE_GetTotalDispensedThisSession(void);
uint32_t MIFARE_GetTotalPurchasedML(void);
uint32_t MIFARE_GetTotalDispensedML(void);

// NEW:
uint32_t MIFARE_GetTokenCount(void);
uint32_t MIFARE_GetLastTopupTokens(void);
uint32_t MIFARE_GetWashTimeRemaining(void);  // Returns remaining seconds
bool MIFARE_IsWashActive(void);              // Returns true if timer running
uint32_t MIFARE_GetTotalTokensPurchased(void);
uint32_t MIFARE_GetTotalWashesCompleted(void);
uint32_t MIFARE_GetTotalWashTimeMinutes(void);
```

---

### 2. MIFARE_Dispenser_Integration.h → (Renamed conceptually to Car Wash)

**File Header**
```c
// OLD:
#ifndef APPLICATION_INCLUDE_MIFARE_DISPENSER_INTEGRATION_H_
#define APPLICATION_INCLUDE_MIFARE_DISPENSER_INTEGRATION_H_

// NEW:
#ifndef APPLICATION_INCLUDE_MIFARE_CARWASH_INTEGRATION_H_
#define APPLICATION_INCLUDE_MIFARE_CARWASH_INTEGRATION_H_
```

**Defines**
```c
// OLD:
#define FLOW_SENSOR_PIN                 22
#define VALVE_CONTROL_PIN               15

// NEW:
#define WASH_BAY_CONTROL_PIN            15
#define WASH_DURATION_SECONDS           (20 * 60)  // 20 minutes
```

**Result Enum**
```c
// OLD:
typedef enum {
    DISPENSER_RESULT_OK = 0,
    DISPENSER_RESULT_INSUFFICIENT_BALANCE,
    DISPENSER_RESULT_INVALID_AMOUNT,
    DISPENSER_RESULT_SENSOR_ERROR,
    DISPENSER_RESULT_FLOW_ERROR,
    // ...
} DispenserResult_t;

// NEW:
typedef enum {
    CARWASH_RESULT_OK = 0,
    CARWASH_RESULT_INSUFFICIENT_TOKENS,
    CARWASH_RESULT_TIMER_ERROR,
    CARWASH_RESULT_TIMER_EXPIRED,
    // ...
} CarWashResult_t;
```

**Status Structure**
```c
// OLD:
typedef struct {
    uint16_t requested_amount_ml;
    uint16_t dispensed_amount_ml;
    bool valve_open;
    uint32_t balance_ml;
    float current_flow_rate_lpm;
    bool flow_detected;
} DispenserStatus_t;

// NEW:
typedef struct {
    bool wash_active;
    uint32_t remaining_seconds;
    uint32_t token_count;
    uint8_t wash_bay_id;
    uint32_t elapsed_seconds;
} CarWashStatus_t;
```

**Function Changes**
```c
// OLD:
DispenserResult_t MIFARE_Dispenser_Init(void);
DispenserResult_t MIFARE_Dispenser_RequestWater(uint16_t amount_ml);
DispenserResult_t MIFARE_Dispenser_GetStatus(DispenserStatus_t *status);
DispenserResult_t MIFARE_Dispenser_EmergencyStop(void);
DispenserResult_t MIFARE_Dispenser_InitializeNewCustomer(uint32_t initial_balance_ml, uint64_t customer_id);
DispenserResult_t MIFARE_Dispenser_TopupCard(uint32_t topup_amount_ml);
void MIFARE_Dispenser_UpdateUI(void);
void MIFARE_Dispenser_ResetTestMode(void);

// NEW:
CarWashResult_t MIFARE_CarWash_Init(void);
CarWashResult_t MIFARE_CarWash_StartWash(void);  // No amount parameter
CarWashResult_t MIFARE_CarWash_GetStatus(CarWashStatus_t *status);
CarWashResult_t MIFARE_CarWash_EmergencyStop(void);
CarWashResult_t MIFARE_CarWash_InitializeNewCustomer(uint32_t initial_tokens, uint64_t customer_id);
CarWashResult_t MIFARE_CarWash_TopupCard(uint32_t topup_tokens);
void MIFARE_CarWash_UpdateUI(void);
void MIFARE_CarWash_ResetTestMode(void);  // Now resets to 10 tokens
```

---

## Implementation Tasks Remaining

The following .c files need to be updated to match the new header definitions:

### 1. MIFARE_Transaction_Manager.c
- Update all state machine references (DISPENSE_STATE → WASH_STATE)
- Replace balance_ml references with token_count
- Update transaction begin logic to deduct 1 token and start 20-min timer
- Implement wash timer logic instead of flow monitoring
- Update all logging messages
- Update CRC and validation functions for new structures
- Implement MIFARE_BeginWashTransaction()
- Implement MIFARE_UpdateWashProgress()
- Implement MIFARE_HandleWashTimerExpired()
- Update getter functions (MIFARE_GetTokenCount, etc.)

### 2. MIFARE_Dispenser_Integration.c (rename to MIFARE_CarWash_Integration.c)
- Remove all flow sensor and valve control code
- Add wash bay control logic
- Implement 20-minute timer management
- Update all function implementations to match new prototypes
- Remove RequestWater, implement StartWash
- Update test mode to use 10 tokens instead of 100L

### 3. System.c
- Update any references to dispenser functions
- Update initialization calls

### 4. UI files (MyWota_ui_driver.c, etc.)
- Update UI display to show tokens instead of mL
- Update display format (e.g., "5 Tokens" instead of "1.5 L")
- Add wash timer countdown display

---

## Backward Compatibility Notes

**Breaking Changes:**
- Format version changed from 0x01 to 0x02
- Old cards with balance_ml data will NOT be compatible
- Card data structure sizes remain the same (16 bytes per block)
- All existing cards will need to be re-initialized or migrated

**Migration Path:**
If you need to migrate old cards:
1. Read old balance_ml value
2. Convert to tokens (e.g., 1000mL = 1 token, or define your own conversion)
3. Write new structure with token_count
4. Update format version to 0x02

---

## Testing Checklist

- [ ] Initialize new card with tokens
- [ ] Scan card and verify token deduction
- [ ] Start 20-minute timer and verify countdown
- [ ] Verify card can be removed after scan
- [ ] Test timer expiration
- [ ] Test top-up functionality
- [ ] Test transaction logging
- [ ] Test card corruption recovery
- [ ] Verify UI displays token count correctly
- [ ] Test emergency stop
- [ ] Verify lifetime statistics tracking

---

## Block Defines Quick Reference

```c
#define MIFARE_BLOCK_HEADER                  4   // Card header
#define MIFARE_BLOCK_USER_PRIMARY            5   // Token count, status
#define MIFARE_BLOCK_USER_BACKUP             6   // Token backup
#define MIFARE_BLOCK_USAGE_DATA              8   // Lifetime wash stats
#define MIFARE_BLOCK_TRANSACTION_LOG         9   // Transaction log
#define MIFARE_BLOCK_RECOVERY_INFO          12   // Recovery data
#define MIFARE_BLOCK_TOKEN_CACHE_PRIMARY    13   // Token cache
#define MIFARE_BLOCK_TOKEN_CACHE_BACKUP     14   // Token cache backup
#define MIFARE_BLOCK_ACCOUNT_DATA           16   // Phone, validity
#define MIFARE_BLOCK_WASH_TIMER_DATA        17   // Wash timer (NEW)
```

---

## Constants

```c
#define WASH_DURATION_SECONDS           (20 * 60)  // 1200 seconds = 20 minutes
```

---

## Summary

The system has been successfully converted from a liquid dispensing system to a car wash token system at the header level. All structure definitions, enums, and function prototypes have been updated. The next step is to update the .c implementation files to match these new definitions.

**Key Behavioral Changes:**
1. Card stores tokens instead of liquid volume
2. Each token = 1 wash session (20 minutes)
3. Card scan triggers immediate token deduction and timer start
4. Card does NOT need to remain in field during wash
5. Timer counts down for 20 minutes
6. Transaction completes when timer expires OR emergency stop is triggered
