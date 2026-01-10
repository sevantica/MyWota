# MIFARE Card Initialization Configuration

## Overview
The MIFARE Transaction Manager now supports configurable card initialization and auto-recovery from SD card configuration files.

## Configuration Parameters

### `mifare.card_init_default_balance_ml`
- **Type:** `uint32_t` (unsigned 32-bit integer)
- **Default:** `10000` (10 liters)
- **Description:** Default balance in milliliters to initialize new or corrupted cards with
- **Example:** `mifare.card_init_default_balance_ml=10000`

### `mifare.auto_reinit_on_corruption`
- **Type:** `bool` (0 or 1)
- **Default:** `1` (enabled)
- **Description:** Enable automatic re-initialization of cards when both primary and backup CRCs are invalid
- **Example:** `mifare.auto_reinit_on_corruption=1`

### `mifare.card_init_phone_number`
- **Type:** `string` (max 11 chars)
- **Default:** `07970242024`
- **Description:** Default contact phone number written to new cards. Used during account data initialization.
- **Example:** `mifare.card_init_phone_number=07123456789`

### `mifare.card_init_validity`
- **Type:** `uint8_t` (0-2)
- **Default:** `2` (CARD_VALIDITY_NORMAL)
- **Description:** Default validity status for new cards.
  - `0`: Suspended
  - `1`: Expired
  - `2`: Normal
- **Example:** `mifare.card_init_validity=2`

### `mifare.no_card_user_id`
- **Type:** `string` (max 31 chars)
- **Default:** `MyWota`
- **Description:** Text displayed on the UI User ID label when no card is present.
- **Example:** `mifare.no_card_user_id=Insert Card`

## How It Works

### 1. **New Card Detection**
When a blank/unformatted card is detected:
- System reads `mifare.card_init_default_balance_ml` from SD config
- Initializes card with configured balance
- Creates proper data structures and CRCs

### 2. **Corrupted Card Auto-Recovery**
When a card with corrupted data is detected:
- If `mifare.auto_reinit_on_corruption=1`:
  - System automatically re-initializes the card
  - Uses `mifare.card_init_default_balance_ml` for new balance
  - Logs recovery action to USB and SD card
- If `mifare.auto_reinit_on_corruption=0`:
  - Card is marked as corrupted
  - No automatic recovery
  - Manual intervention required

### 3. **Configuration Loading**
Configuration is loaded in the following priority:
1. SD card config file (`0:/config.txt`)
2. Flash backup (if SD unavailable)
3. Hardcoded defaults (if both unavailable)

## Configuration File Format

Example `config.txt` on SD card:
```
# MIFARE Card Initialization Settings
mifare.card_init_default_balance_ml=10000
mifare.auto_reinit_on_corruption=1
```

## Usage Examples

### Example 1: Development Environment
For testing with minimal balance:
```
mifare.card_init_default_balance_ml=1000
mifare.auto_reinit_on_corruption=1
```

### Example 2: Production Environment
For production with auto-recovery disabled:
```
mifare.card_init_default_balance_ml=50000
mifare.auto_reinit_on_corruption=0
```

### Example 3: Field Deployment
For field units with aggressive recovery:
```
mifare.card_init_default_balance_ml=25000
mifare.auto_reinit_on_corruption=1
```

## Logging

The system logs card initialization activities:

**USB Logs:**
```
MIFARE: Auto-init config: balance=10000 mL, auto_reinit=enabled
MIFARE: Blank/Unformatted card detected, initializing with 10000 mL...
MIFARE: Card initialization completed successfully
```

**Auto-Recovery Logs:**
```
MIFARE: Card data corrupted - both CRCs invalid. Auto-reinit enabled - Re-initializing card...
MIFARE: Auto-recovery triggered - re-initializing corrupted card (balance: 10000 mL)
```

**Auto-Recovery Disabled Logs:**
```
MIFARE: Card data corrupted but auto-reinit is DISABLED. Card marked as corrupted.
MIFARE: Card corrupted - auto-reinit disabled in config
```

## Files Modified

1. **System_Config.h** - Added new MIFARE config fields
2. **System_Config.c** - Added parsing, writing, and default values
3. **MIFARE_Transaction_Manager.c** - Integrated SD card config reading

## Benefits

✅ **Flexible Deployment** - Different sites can have different initialization policies
✅ **No Code Changes** - Configuration via SD card, no firmware rebuild needed
✅ **Automatic Recovery** - Corrupted cards can be automatically recovered
✅ **Safety Options** - Can disable auto-recovery for sensitive deployments
✅ **Audit Trail** - All initializations and recoveries are logged
✅ **Easy Testing** - Quick balance adjustments for testing scenarios

## Safety Considerations

⚠️ **Important:** 
- Auto-reinit will reset a corrupted card to the default balance
- Original balance data is lost if both CRCs are invalid
- Consider disabling auto-reinit in production if data preservation is critical
- Always check SD card logs for recovery events

## Migration Guide

If upgrading from a version without this feature:

1. System will use defaults if no config exists
2. To customize, edit `0:/config.txt` on SD card
3. Add the two new parameters shown above
4. Restart system to load new config
5. Config is also backed up to flash automatically

## See Also

- `example_config.txt` - Full configuration example
- `System_Config.h` - Full configuration structure
- `MIFARE_Transaction_Manager.c` - Implementation details
