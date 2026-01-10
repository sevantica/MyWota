# PBKDF2 Watchdog Timeout Fix

## Problem
PBKDF2 key derivation with 10,000 iterations was taking **~3 seconds** on the RP2040 (Cortex-M0+ @ 133MHz), blocking the MIFARE task and preventing watchdog feeding:

```
[MIFARE-SEC] Running PBKDF2 with 10000 iterations...
[WDT] Not all tasks reported within 2400ms:
  MIFARE: NO_REPORT (last: 2908 ms ago)
```

## Solutions Implemented

### 1. Reduced Default Iterations (Primary Fix)
**Changed**: `mifare.security.pbkdf2_iterations=1000` (was 10000)

**Performance**:
| Iterations | Time | Security Level |
|-----------|------|----------------|
| 1,000 | ~300ms | ✅ Acceptable for embedded |
| 2,000 | ~600ms | ✅ Good for embedded |
| 5,000 | ~1.5s | ⚠️ May cause WDT warnings |
| 10,000 | ~3.0s | ❌ Blocks watchdog |

**Recommendation**: Use **1000-2000** iterations for embedded systems. This still provides strong security while maintaining responsive operation.

### 2. Added Watchdog Feeding (Backup Protection)
**Modified**: `SHA256_Crypto.c` - Added `watchdog_update()` every 100 PBKDF2 iterations

This ensures even if someone configures high iteration counts, the watchdog won't trigger a reset during key derivation.

```c
// Feed watchdog every 100 iterations (~30ms) to prevent WDT reset
#ifdef PICO_DEFAULT_WDG_UPDATE
if ((iter % 100) == 0) {
    watchdog_update();
}
#endif
```

## Security Analysis

### Is 1000 Iterations Secure Enough?

**YES** for this use case:

1. **Per-Card Unique Keys**: Each card gets a unique derived key based on:
   - Master secret (256-bit)
   - Card UID (4-7 bytes, unique per card)
   - Device unique ID (8 bytes, burned in RP2040)

2. **Attack Scenario**: Attacker would need:
   - Physical access to card
   - Knowledge of master key (stored on SD card)
   - Brute force PBKDF2 (1000 iterations)

3. **Brute Force Time** (assuming attacker has master key):
   - AES-128 keyspace: 2^128 keys
   - Even with 1 iteration: Billions of years to brute force
   - PBKDF2 iterations protect against **weak password** attacks
   - Our "password" is 256-bit random key (not weak!)

4. **Comparative Security**:
   - WPA2-PSK: Uses 4096 iterations (wireless passwords)
   - 1Password: Uses 100,000 iterations (user passwords)
   - **Difference**: Those protect human-chosen passwords (weak)
   - **Our case**: Protects 256-bit random keys (already strong)

### Bottom Line
With a strong 256-bit master key, **1000 iterations is sufficient**. The PBKDF2 iterations mainly add computational cost, which matters more when the input is a weak password. Since we're using high-entropy random keys, 1000 iterations provides adequate security while maintaining real-time responsiveness.

## Configuration Update Required

**Action**: Update your SD card config file `config_v1.txt`:

```ini
# Change this line from 10000 to 1000:
mifare.security.pbkdf2_iterations=1000
```

Or copy the updated `example_security_config.txt` to your SD card.

## Testing Results

After updating config and reflashing:
- ✅ Card detection: ~300ms (down from 3000ms)
- ✅ No watchdog warnings
- ✅ All tasks report within 2400ms window
- ✅ Encryption/decryption still working correctly

## Alternative Solutions (Not Implemented)

### If You Need More Iterations

If regulatory/compliance requires higher iteration counts:

1. **Async Key Derivation** (complex):
   - Move PBKDF2 to separate low-priority task
   - Use semaphores to signal completion
   - Card operations wait for key derivation

2. **Hardware Acceleration** (requires hardware):
   - Use DMA for AES operations
   - Offload to crypto coprocessor (e.g., ATECC608)

3. **Pre-Computed Keys** (security tradeoff):
   - Store derived keys in flash
   - Skip PBKDF2 on subsequent card detections
   - Risk: Keys persist in flash

For this application, **reducing iterations to 1000-2000 is the best solution**.

## Files Modified

1. [example_security_config.txt](../application/example_security_config.txt) - Default iterations: 10000 → 1000
2. [SHA256_Crypto.c](../../../Common/sevantica_drivers/Source/SHA256_Crypto.c) - Added watchdog feeding
3. [SHA256_Crypto.c](../../../Common/sevantica_drivers/Source/SHA256_Crypto.c) - Added `hardware/watchdog.h` include

## Summary

**Problem**: 10,000 PBKDF2 iterations blocked watchdog for 3 seconds  
**Solution**: Reduced to 1000 iterations (~300ms) + added watchdog feeding  
**Security**: Still strong due to 256-bit random master key (not weak password)  
**Result**: ✅ No watchdog timeouts, fast card detection, secure encryption  

---
**Document Version**: 1.0  
**Date**: 2025-12-29  
**Status**: ✅ Fixed and tested
