# Car Wash System Architecture

## Design Philosophy: Polling Architecture

This system follows a **polling architecture** where higher-level modules poll lower-level modules for data, rather than lower-level modules pushing events upward.

---

## Layer Architecture

```
┌─────────────────────────────────────────────────────┐
│                     UI Layer                        │
│          (MyWota_ui_driver.c)                    │
│   Polls CarWash layer for display data              │
└─────────────────────────────────────────────────────┘
                        ▲
                        │ polls
                        │
┌─────────────────────────────────────────────────────┐
│              Business Logic Layer                   │
│        (MIFARE_CarWash_Integration.c)               │
│                                                     │
│  • Manages wash timer (20 minutes)                 │
│  • Decides when to deduct tokens                   │
│  • Polls Transaction Manager for card data         │
│  • Modifies card data (token_count, etc.)          │
│  • Tells Transaction Manager to write to card      │
└─────────────────────────────────────────────────────┘
                        ▲
                        │ polls
                        │
┌─────────────────────────────────────────────────────┐
│           Card I/O Layer (Generic)                  │
│       (MIFARE_Transaction_Manager.c)                │
│                                                     │
│  • Generic state machine (card detected, reading,  │
│    writing, error, etc.)                           │
│  • Manages card data structures                    │
│  • Provides getters/setters for card data          │
│  • Handles card authentication and I/O             │
│  • NO BUSINESS LOGIC - doesn't know what tokens    │
│    are or what they mean                           │
└─────────────────────────────────────────────────────┘
                        ▲
                        │
┌─────────────────────────────────────────────────────┐
│              Hardware Driver Layer                  │
│             (PN532_Driver.c)                        │
│                                                     │
│  • Low-level NFC/RFID communication                │
│  • I2C protocol handling                           │
└─────────────────────────────────────────────────────┘
```

---

## Module Responsibilities

### 1. MIFARE_Transaction_Manager (Card I/O Layer)

**Purpose:** Generic MIFARE card read/write operations - NO business logic

**Responsibilities:**
- Detect card presence/removal
- Authenticate with card sectors
- Read card blocks into memory structures
- Write memory structures to card blocks
- Validate data integrity (CRC checks)
- Handle card removal/errors gracefully
- Provide **getters** for card data structures
- Provide **setters** for card data structures (business logic layer updates via setters)

**State Machine (Generic):**
```
TRANSACTION_STATE_IDLE              → Card not present
TRANSACTION_STATE_CARD_DETECTED     → Card detected, not yet authenticated
TRANSACTION_STATE_AUTHENTICATING    → Authenticating with card
TRANSACTION_STATE_READING_DATA      → Reading card blocks
TRANSACTION_STATE_VALIDATING        → Validating data integrity
TRANSACTION_STATE_READY             → Card data valid, ready for use
TRANSACTION_STATE_WRITING_DATA      → Writing to card
TRANSACTION_STATE_FINALIZING        → Finalizing transaction
TRANSACTION_STATE_ERROR             → Error occurred
TRANSACTION_STATE_CARD_REMOVED      → Card removed
```

**Key Functions:**
```c
// Card lifecycle
MIFARE_Result_t MIFARE_TransactionManager_Init(void);
MIFARE_Result_t MIFARE_ProcessCardDetected(PN532_CardInfo_t *card_info);
MIFARE_Result_t MIFARE_ProcessCardRemoved(void);

// Card I/O operations
MIFARE_Result_t MIFARE_ReadCardData(MIFARE_CardData_t *card_data);
MIFARE_Result_t MIFARE_WriteCardData(MIFARE_CardData_t *card_data);
MIFARE_Result_t MIFARE_ValidateCardData(MIFARE_CardData_t *card_data);

// Atomic transactions
MIFARE_Result_t MIFARE_BeginTransaction(void);      // Lock for atomic write
MIFARE_Result_t MIFARE_UpdateCardData(void);        // Write in-memory data to card
MIFARE_Result_t MIFARE_CommitTransaction(void);     // Finalize transaction
MIFARE_Result_t MIFARE_RollbackTransaction(void);   // Rollback on failure

// Data accessors (getters) - business logic polls these
MIFARE_UserData_t* MIFARE_GetUserData(void);           // Get token_count, etc.
MIFARE_UsageData_t* MIFARE_GetUsageData(void);         // Get lifetime stats
MIFARE_WashTimerData_t* MIFARE_GetWashTimerData(void); // Get timer data
MIFARE_AccountData_t* MIFARE_GetAccountData(void);     // Get phone/validity

// Data accessors (setters) - business logic updates via these
void MIFARE_SetUserData(const MIFARE_UserData_t *user_data);
void MIFARE_SetUsageData(const MIFARE_UsageData_t *usage_data);
void MIFARE_SetWashTimerData(const MIFARE_WashTimerData_t *timer_data);

// State queries
MIFARE_TransactionState_t MIFARE_GetTransactionState(void);
bool MIFARE_IsCardPresent(void);
bool MIFARE_IsCardReady(void);  // Card present and data valid
```

**What it does NOT do:**
- ❌ Know what tokens are for
- ❌ Decide when to deduct tokens
- ❌ Manage wash timers
- ❌ Make business decisions
- ❌ Call UI functions

---

### 2. MIFARE_CarWash_Integration (Business Logic Layer)

**Purpose:** Car wash business logic - manages tokens, timers, and wash sessions

**Responsibilities:**
- Poll transaction manager for card status
- Read token count from card via getter
- Decide when to start a wash (token available + user request)
- Deduct 1 token when wash starts
- Start and manage 20-minute wash timer
- Update wash timer data on card
- Track lifetime statistics (total washes, total tokens, total time)
- Write updated data to card via transaction manager setters
- Provide UI-friendly getter functions for display

**State Machine (Wash-Specific):**
```c
typedef enum {
    CARWASH_IDLE,
    CARWASH_WAITING_FOR_CARD,
    CARWASH_CARD_READY,
    CARWASH_STARTING_WASH,      // Deducting token
    CARWASH_WASH_IN_PROGRESS,   // Timer counting down
    CARWASH_FINALIZING,
    CARWASH_ERROR
} CarWashState_t;
```

**Key Functions:**
```c
// Initialization
CarWashResult_t MIFARE_CarWash_Init(void);

// Wash operations
CarWashResult_t MIFARE_CarWash_StartWash(void);  // Checks tokens, deducts 1, starts timer
CarWashResult_t MIFARE_CarWash_GetStatus(CarWashStatus_t *status);
CarWashResult_t MIFARE_CarWash_EmergencyStop(void);

// Card management
CarWashResult_t MIFARE_CarWash_InitializeNewCustomer(uint32_t initial_tokens, uint64_t customer_id);
CarWashResult_t MIFARE_CarWash_TopupCard(uint32_t topup_tokens);

// Polling task (runs periodically)
void MIFARE_CarWash_Task(void* argument);
void Task_Start_CarWash_Task(void);

// UI getter functions - UI polls these
uint32_t CarWash_GetTokenCount(void);
uint32_t CarWash_GetWashTimeRemaining(void);
bool CarWash_IsWashActive(void);
uint32_t CarWash_GetTotalWashesCompleted(void);
uint32_t CarWash_GetTotalTokensPurchased(void);
```

**Typical Wash Flow:**
1. **Poll** transaction manager: Is card ready?
2. **Read** token count via `MIFARE_GetUserData()->token_count`
3. **Check** if tokens > 0
4. **User presses "Start Wash"**
5. **Deduct** 1 token: `user_data->token_count--`
6. **Update** wash timer: `wash_timer->wash_active = 1; wash_timer->wash_start_time = now();`
7. **Write** to card via `MIFARE_SetUserData()`, `MIFARE_SetWashTimerData()`, `MIFARE_UpdateCardData()`
8. **Start** 20-minute countdown timer
9. **Every second**, update timer display (via UI polling)
10. **When timer expires**, mark wash complete, update statistics, write to card

**What it does NOT do:**
- ❌ Call UI functions directly
- ❌ Directly access PN532 driver
- ❌ Manage card authentication/I/O

---

### 3. UI Layer (MyWota_ui_driver.c)

**Purpose:** Display information to user

**Responsibilities:**
- Poll CarWash layer for display data
- Update UI elements (token count, timer, status)
- Handle user button presses
- Request wash start from CarWash layer

**Polling Functions (called periodically):**
```c
void UI_Update_Task(void) {
    // Poll business logic layer
    uint32_t tokens = CarWash_GetTokenCount();
    uint32_t time_remaining = CarWash_GetWashTimeRemaining();
    bool wash_active = CarWash_IsWashActive();
    
    // Update display
    lv_label_set_text_fmt(ui_tokenCount, "%lu Tokens", tokens);
    
    if (wash_active) {
        uint32_t minutes = time_remaining / 60;
        uint32_t seconds = time_remaining % 60;
        lv_label_set_text_fmt(ui_timer, "%02lu:%02lu", minutes, seconds);
    }
}
```

**What it does NOT do:**
- ❌ Read card data directly
- ❌ Manage wash timers
- ❌ Make business decisions

---

## Data Flow Examples

### Example 1: Starting a Wash

```
User presses "Start Wash" button
    ↓
UI calls → CarWash_StartWash()
    ↓
CarWash polls → MIFARE_GetUserData()
    ↓
CarWash checks → user_data->token_count > 0?
    ↓ Yes
CarWash modifies → user_data->token_count--
CarWash modifies → wash_timer->wash_active = 1
CarWash modifies → wash_timer->wash_start_time = now()
    ↓
CarWash calls → MIFARE_SetUserData(user_data)
CarWash calls → MIFARE_SetWashTimerData(wash_timer)
CarWash calls → MIFARE_BeginTransaction()
CarWash calls → MIFARE_UpdateCardData()  // Writes to card
CarWash calls → MIFARE_CommitTransaction()
    ↓
CarWash starts local timer (20 minutes)
    ↓
Returns success to UI
```

### Example 2: UI Updating Display

```
UI_Update_Task() runs every 100ms
    ↓
UI polls → CarWash_GetTokenCount()
    ↓
CarWash polls → MIFARE_GetUserData()
    ↓
CarWash returns → user_data->token_count
    ↓
UI updates display → "5 Tokens"

UI polls → CarWash_GetWashTimeRemaining()
    ↓
CarWash calculates → (20*60) - (now() - wash_start_time)
    ↓
CarWash returns → 1145 seconds
    ↓
UI updates display → "19:05"
```

### Example 3: Card Topup

```
User scans card with tokens to add
    ↓
System calls → CarWash_TopupCard(10)
    ↓
CarWash polls → MIFARE_GetUserData()
    ↓
CarWash modifies → user_data->token_count += 10
CarWash modifies → user_data->last_topup_tokens = 10
CarWash modifies → usage_data->total_tokens_purchased += 10
    ↓
CarWash calls → MIFARE_SetUserData(user_data)
CarWash calls → MIFARE_SetUsageData(usage_data)
CarWash calls → MIFARE_BeginTransaction()
CarWash calls → MIFARE_UpdateCardData()  // Writes to card
CarWash calls → MIFARE_CommitTransaction()
    ↓
Returns success
```

---

## Key Principles

### 1. **Separation of Concerns**
- **Transaction Manager**: Card I/O only, no business logic
- **CarWash Integration**: Business logic only, no direct hardware access
- **UI**: Display only, no business decisions

### 2. **Polling, Not Pushing**
- UI **polls** CarWash for data
- CarWash **polls** Transaction Manager for data
- No callbacks, no events pushed upward

### 3. **Data Ownership**
- Transaction Manager **owns** card data structures in memory
- Business logic layer **reads** via getters
- Business logic layer **modifies** via setters
- Transaction Manager **writes** to physical card

### 4. **Generic Core**
- Transaction Manager is **application-agnostic**
- Could be reused for other applications (parking, vending, etc.)
- Only card data structures are application-specific

---

## Benefits of This Architecture

✅ **Testability**: Each layer can be tested independently
✅ **Reusability**: Transaction Manager can be reused for different applications
✅ **Maintainability**: Clear responsibilities, easy to understand
✅ **Flexibility**: Easy to add new business logic without touching card I/O
✅ **Debugging**: Easy to isolate issues to specific layers
✅ **No Circular Dependencies**: Clean unidirectional data flow

---

## Task Configuration

### CarWash Task (Business Logic)
- **Stack Size**: 512 words (adjust based on needs)
- **Priority**: `tskIDLE_PRIORITY + 1`
- **Function**: `MIFARE_CarWash_Task()`
- **Responsibilities**: 
  - Poll transaction manager every 100ms
  - Update wash timer countdown
  - Write timer updates to card every 5 seconds
  - Handle wash completion
  - Update lifetime statistics

### Card Polling Task (Transaction Manager)
- **Stack Size**: 512 words
- **Priority**: `tskIDLE_PRIORITY + 2` (higher than business logic)
- **Function**: `MIFARE_PollingTask()`
- **Responsibilities**:
  - Poll PN532 for card presence
  - Read card data when detected
  - Validate card data integrity
  - Handle card removal

---

## Summary

The key insight is that **MIFARE_Transaction_Manager is a generic card I/O library**, not a business-logic-aware component. The business logic lives in **MIFARE_CarWash_Integration**, which polls the transaction manager for data, makes decisions, and tells the transaction manager what to write back to the card.

This creates a clean, maintainable architecture where each layer has a single, well-defined responsibility.
