# MyWota Feature Backlog

Each item is sized so it can be picked up independently in its own session.
Order is rough priority (top = highest ROI). Update status as we go.

Status legend: `[ ]` not started · `[~]` in progress · `[x]` done · `[-]` deferred

---

## Recently completed (reference)

- [x] **Self-clean cycle (CCH-orchestrated)** — periodic per-slave flush, one
  unit at a time for full booster pressure. See
  `/memories/session/self_clean_implementation_done.md`.

---

## Tier 1 — Operational reliability

### [x] 1. Filter life tracking
- **Why:** Cheap, customer-visible, prevents bad-tasting-water complaints.
- **Scope:**
  - Persist cumulative `liters_since_filter_change` in `MIFARE_UsageData_t`
    or a new `Maintenance_Counters_t` flash record.
  - Add `filter.expected_lifetime_l` to config (default 5000 L).
  - UI: show "Filter: 78%" or remaining liters on idle screen.
  - RS485 status: extra field `filter_remaining_pct`.
  - USB commands: `filter status`, `filter reset`.
- **Touches:** Dispenser_Controller (increment on completed dispense),
  System_Config schema, mywota_ui_driver, RS485_Protocol status struct,
  USB_Command_Adapter.
- **Effort:** S–M.

### [ ] 2. Tamper / case-open detection
- **Why:** One GPIO, huge deterrent. Latched alarm.
- **Scope:**
  - Free GPIO with pull-up + reed/limit switch input.
  - Debounce + latch on the slave; flag clears only via authenticated USB
    or RS485 command.
  - New `RS485_STATUS_FLAG_TAMPER` bit.
  - Buzzer pattern + UI banner.
- **Touches:** Hardware_Access pins, RS485_Protocol flags, Dispenser/
  System_Status, mywota_ui_driver, USB_Command_Adapter (`tamper clear`).
- **Effort:** S.

### [ ] 3. Flow-sensor calibration & drift detection
- **Why:** YS-S201 is part-to-part variable; today we trust nominal pulses/L.
- **Scope:**
  - `flow.calibration_factor` (float) in config, applied in YS_S201_Driver.
  - `calibrate <known_volume_ml>` USB command: opens valve until user types
    `done`, computes factor.
  - During normal dispense, if observed `flow_rate_lpm` drops > X% from
    rolling baseline, set `RS485_STATUS_FLAG_FLOW_ANOMALY`.
- **Touches:** YS_S201_Driver, Dispenser_Controller, System_Config,
  RS485_Protocol, USB_Command_Adapter.
- **Effort:** M.

### [x] 4. Per-unit fault state machine + telemetry alerts
- **Why:** `error_code` exists but no persistent fault tracking; CCH should
  raise alerts on stuck conditions.
- **Scope:**
  - States: OK / DEGRADED / FAULT, plus reason enum (no_flow, valve_stuck,
    sensor_lost, repeated_auth_fail, tamper, watchdog_reboots).
  - Latched until cleared; reported in RS485 status.
  - CCH pushes faulty units to telemetry server with severity.
- **Touches:** new `Fault_Manager.c` (slave) + CCH telemetry path.
- **Effort:** M.

---

## Tier 2 — Business / commercial

### [ ] 5. Loyalty wiring (finish what's already in config)
- **Why:** `loyalty_threshold` / `loyalty_reward` already in
  `DispenserLogic_Config_t` but unused on MyWota.
- **Scope:**
  - In `MIFARE_Volume_Adapter`, on dispense complete: accumulate per-card
    purchased volume, when threshold crossed credit reward mL.
  - Persist counter in card user data area (already encrypted).
  - UI toast: "Loyalty reward: +20 mL".
- **Touches:** MIFARE_Volume_Adapter, MIFARE_UserData_t schema, UI.
- **Effort:** S–M.

### [ ] 6. Pricing tiers & promotions
- **Why:** Today 1 mL = 1 unit; no pricing flexibility.
- **Scope:**
  - Price table in CCH config: per-mL rate, bulk discount thresholds,
    time-of-day pricing.
  - CCH pushes table to slaves via new RS485 command on connect / config
    change.
  - Slave applies during top-up calculation.
- **Touches:** System_Config (CCH), RS485_Protocol (new
  CMD_SET_PRICING), MIFARE_Transaction_Core.
- **Effort:** M.

### [ ] 7. Pre-paid one-time tokens (QR voucher)
- **Why:** Lower friction for occasional users (no MIFARE card to issue).
- **Scope:**
  - Generate signed vouchers server-side; redeem via small keypad or USB
    command (v2: camera-based QR).
  - Voucher = `{voucher_id, mL, expiry, signature}`; slave verifies HMAC.
  - Voucher list synced via CCH; redemption logged + broadcast to mark
    consumed.
- **Touches:** new `Voucher_Manager.c`, MIFARE-equivalent transaction path,
  CCH telemetry, hardware (keypad).
- **Effort:** L.

### [ ] 8. Anonymous receipt QR
- **Why:** Customers without a card can prove what they got; pairs with
  vouchers and disputes.
- **Scope:**
  - At dispense end, generate
    `{txn_id, vol_ml, ts, slave_uid, signature}` and render as QR on LCD
    for ~10 s.
  - Reuses existing crypto stack.
- **Touches:** UI (QR widget already in LVGL?), Dispenser_Controller end
  hook, signing key in security config.
- **Effort:** M.

---

## Tier 3 — Ops visibility

### [ ] 9. Audit log on SD (or flash if no SD)
- **Why:** Indispensable for chargebacks and field forensics. We currently
  have no per-event durable log.
- **Scope:**
  - Append CSV line per: dispense, top-up, fault, clean cycle, config
    change, tamper.
  - Format:
    `iso_ts,slave,uid,event,vol_ml,balance_before,balance_after,checksum`.
  - Daily file rotation; CCH can pull via existing file-transfer path.
- **Touches:** SD_Logger_Task, hooks in Dispenser/MIFARE/Config/RS485
  modules.
- **Effort:** M.

### [x] 10. Daily/hourly throughput counters
- **Why:** Capacity planning and revenue dashboards.
- **Scope:**
  - Ring buffer in flash: 24 hourly slots × {liters, txns, peak_lpm}.
  - Rolls over at midnight; CCH pulls and forwards to telemetry.
- **Touches:** new `Throughput_Stats.c`, RS485_Protocol (CMD_GET_STATS),
  CCH telemetry.
- **Effort:** M.

### [ ] 11. Local web dashboard on CCH (Pico W)
- **Why:** Turns the fleet into something manageable without needing the
  cloud.
- **Scope:**
  - CCH already has `web_server` skeleton. Endpoints:
    - `GET /status` → all slaves' live state JSON.
    - `GET /stats` → throughput counters.
    - `POST /clean` → manual self-clean trigger (reuses Clean_Scheduler).
    - `POST /fwupdate` → orchestrates fw flow.
  - Plain HTML/JS, internal LAN only.
- **Touches:** Central-Control-Hub web_server, Clean_Scheduler glue,
  RS485 master status accessors.
- **Effort:** M–L.

### [ ] 12. Remote firmware update orchestration
- **Why:** RS485_FW_* exists but flow is fragmented. Need a one-shot
  command with progress + auto-rollback.
- **Scope:**
  - CCH command `fwupdate <addr> <slot>`:
    1. CMD_FW_START (size).
    2. CMD_FW_DATA chunks (progress reported).
    3. CMD_FW_VERIFY → if fail, abort & alert.
    4. CMD_FW_APPLY → schedule reboot.
    5. Watch slave re-discovery + version field; rollback if mismatch.
  - Bootloader rollback already supported; just wire the master flow.
- **Touches:** Central-Control-Hub task, USB_Command_Adapter, web_server.
- **Effort:** M.

### [ ] 13. Time-window operating hours
- **Why:** Noise / security in residential locations.
- **Scope:**
  - Config: `operating_hours.{start_hour, end_hour}`; if start==end,
    always on.
  - Slave-side gate in Dispenser_Controller (refuse new dispense outside
    window; finish in-progress one).
  - UI toast: "Closed until 06:00".
- **Touches:** System_Config, Dispenser_Controller, UI.
- **Effort:** S.

---

## Tier 4 — Security / abuse mitigation

### [ ] 14. Card blacklist / hotlist
- **Why:** Counters lost or stolen cards.
- **Scope:** CCH pushes ≤256 UIDs to slaves; MIFARE auth path refuses
  match. Synced on connect and on `blacklist add/remove` USB command.
- **Effort:** S–M.

### [x] 15. Per-card daily rate limit
- **Why:** Prevents abuse / commercial reselling on consumer cards.
- **Scope:** Config `mifare.max_daily_volume_ml`; track midnight-reset
  counter on card user data; refuse beyond limit.
- **Effort:** S.

### [ ] 16. Card cloning detection (server-side counter)
- **Why:** HMAC + replay are local; cross-slave cloning still possible.
- **Scope:** CCH tracks `(uid, monotonic_txn_counter)` watermark; on
  out-of-order counter from any slave, blacklist UID and alert.
- **Effort:** M (needs telemetry round-trip).

---

## Tier 5 — Nice to have / longer term

### [ ] 17. Periodic line drain / freeze protection
- **Scope:** Sister cycle to self-clean. New `CMD_TRIGGER_DRAIN`,
  triggered by configured low-temp window. Sensor: existing on-die or
  add a 1-Wire DS18B20.
- **Effort:** M (mostly mirrors Clean_Scheduler).

### [ ] 18. Sleep / low-power mode
- **Scope:** LCD off after idle timeout, MIFARE polling at 1 Hz, wake on
  capacitive proximity / button. Meaningful for solar sites.
- **Effort:** M.

### [ ] 19. Voice prompts
- **Scope:** Replace buzzer with small WAV player; multilingual prompts
  ("Tap card", "Insufficient balance"). Hardware change.
- **Effort:** L.

### [ ] 20. Dispense scheduling / app pickup
- **Scope:** Customer queues volume in app, picks up via QR scan. Heavy
  UX + back-end work; revisit when fleet is larger.
- **Effort:** XL.

### [ ] 21. Air-purge / line-blow
- **Scope:** Out of scope without hardware redesign (compressor).
- **Effort:** N/A — hardware decision first.

---

## Suggested next 5 (by ROI)

1. Filter life tracking (#1)
2. Tamper detection (#2)
3. Audit log (#9)
4. Loyalty wiring (#5)
5. Local web dashboard (#11)
