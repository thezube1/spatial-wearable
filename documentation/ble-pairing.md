# BLE Pairing Protocol

The Spatial Wearable exposes its STA MAC address over a read-only GATT characteristic so the iOS companion app can link a specific physical wearable to the current user during onboarding.

Firmware: `arduino/10_wearable_persistent_pairing/10_wearable_persistent_pairing.ino` (forked from `09_wearable_pairing`).

## GATT Layout

| Item | UUID | Properties | Value |
|---|---|---|---|
| Service | `12345678-1234-5678-1234-56781234abcd` | -- | Primary service; also advertised |
| MAC characteristic | `12345678-1234-5678-1234-56781234abce` | READ | 17-byte ASCII MAC. Gated: returns empty when the wearable is paired and the current central has not authed. |
| Owner-write | `12345678-1234-5678-1234-56781234abcf` | WRITE | Accepted ONLY while the wearable is in pairing mode. iOS writes the Supabase `user_id` (UUID string, UTF-8). Firmware stores it in NVS and exits pairing mode. |
| Owner-auth | `12345678-1234-5678-1234-56781234abd0` | WRITE | Required on every reconnect once paired. iOS writes the same `user_id`; firmware compares to NVS and disconnects on mismatch. Centrals that don't write within 3 s of connecting are also disconnected. |
| Location | `12345678-1234-5678-1234-56781234abd1` | READ, NOTIFY | 9-byte GPS payload pushed by the wearable every 15 s. Layout: `valid(u8) | lat(float32 LE) | lon(float32 LE)`. `valid=0` means no fix yet — lat/lon should be ignored. Added in firmware `12_wearable_location_ble`. |
| Target-select | `12345678-1234-5678-1234-56781234abd2` | WRITE | Only accepted on an authed connection. iOS writes the group member the wearable should lock onto. Added in firmware `13_wearable_target_select`. |
| Peer-location | `12345678-1234-5678-1234-56781234abd3` | READ, NOTIFY | 9-byte payload carrying the *target* peer's GPS forwarded from ESP-NOW. Same layout as Location (`valid(u8) | lat(f32 LE) | lon(f32 LE)`). Notified every ~3 s, and immediately when the target changes or clears (valid=0). Added in firmware `14_wearable_peer_location`. |
| Candidate list | `12345678-1234-5678-1234-56781234abd4` | WRITE | Owner-auth gated. iOS pushes up to 8 group members the wearer can cycle through and confirm on the watch. Wire format: `[u8 count]` then per entry `[6-byte MAC][u8 nameLen][nameLen UTF-8]`. `count=0` clears. Added in firmware `16_wearable_group_targets`. |
| Selected target | `12345678-1234-5678-1234-56781234abd5` | READ, NOTIFY | 6-byte MAC of the wearable's currently-tracked target; all-`0xFF` means "no target". Source of truth for the iOS Group screen badge. Notified on watch long-press confirm, on phone `abd2` writes (mirrored), and on stale-target prune. Added in firmware `16_wearable_group_targets`. |
| GPS sync | `12345678-1234-5678-1234-56781234abd6` | READ, WRITE, NOTIFY | Owner-auth gated. iOS writes a single `0x01` byte to ask the wearable to enter a 30-second focused-fix mode (radios off). Wearable notifies a 2-byte payload `[status(u8), secondsRemaining(u8)]` reporting state transitions: `0x01`=running, `0x02`=success (fresh fix acquired), `0x03`=failed (timed out). Added in firmware `18_wearable_gps_sync`. |

## Persistent State (NVS)

Firmware 10 uses the `Preferences` namespace `sw-pair` with two keys:

- `paired` (bool) — true once an owner is saved.
- `owner` (string) — Supabase `user_id` UUID of the linked account.

Fresh devices boot with `paired=false` and therefore start in pairing mode. Holding the BOOT button (GPIO0) for 5 seconds calls `prefs.clear()` and re-enters pairing mode, forgetting the prior owner. After a successful `owner-write`, the wearable boots directly into normal proximity mode on subsequent power-cycles.

The service UUID is the same one already used by firmware 08 for peer discovery, so the existing advertising path is reused unchanged. Only the new MAC characteristic is added.

## Value Format

The MAC characteristic holds exactly 17 ASCII bytes in the canonical uppercase colon-separated form:

```
AA:BB:CC:DD:EE:FF
```

Source: `esp_wifi_get_mac(WIFI_IF_STA, myMac)`, formatted via `snprintf("%02X:%02X:%02X:%02X:%02X:%02X", ...)`. The value is set once in `setup()` immediately after the service is created; there are no notifications and no writes.

The string is also used verbatim as the primary key (`devices.mac_address`) in the backend.

## Advertising

- Local name: `SW-XXXX` where `XXXX` is the last 4 hex digits of the STA MAC (upper-case, no separators).
- Service UUID in advertising payload: the service UUID above.
- Scan response: enabled.

Scanning clients should filter on the service UUID, not on the local name, to remain compatible if the name format changes.

## iOS Pairing Sequence

1. **Scan** -- `CBCentralManager.scanForPeripherals(withServices: [SERVICE_UUID])` with `CBCentralManagerScanOptionAllowDuplicatesKey = false`. Show a "Searching..." state.
2. **Select** -- Pick the strongest RSSI advertisement (user is expected to be holding the wearable). Stop scanning.
3. **Connect** -- `central.connect(peripheral)`.
4. **Discover service** -- `peripheral.discoverServices([SERVICE_UUID])`.
5. **Discover characteristic** -- `peripheral.discoverCharacteristics([MAC_CHAR_UUID], for: service)`.
6. **Read** -- `peripheral.readValue(for: macCharacteristic)`. The delegate returns 17 ASCII bytes; decode as UTF-8.
7. **Validate** -- Reject if length != 17 or if the string does not match the regex `^[0-9A-F]{2}(:[0-9A-F]{2}){5}$`.
8. **Link** -- `POST /devices/link` with body `{"mac": "AA:BB:CC:DD:EE:FF"}` and `Authorization: Bearer <supabase_jwt>`. Backend inserts or updates `public.devices.linked_user_id` and returns the canonical device row.
9. **Disconnect or keep open** -- Either `central.cancelPeripheralConnection(peripheral)` once the POST returns 200, or keep the connection open if the dashboard will immediately begin streaming proximity data. Either is acceptable; the firmware holds no per-connection state beyond the standard GATT session.

## Error Handling

| Condition | UX |
|---|---|
| No advertisement seen within 15s | "Couldn't find a wristband. Make sure it's powered on." Retry button. |
| Connect timeout (CoreBluetooth ~10s) | Retry once, then surface the error. |
| Characteristic read fails | Disconnect, retry from step 3 up to 3 times. |
| `/devices/link` returns 409 (mac already linked to another user) | Surface "This wristband is already linked to another account." |
| `/devices/link` returns 5xx | Retry with exponential backoff; keep the MAC cached so the user doesn't need to re-scan. |

## Firmware Reference

In `09_wearable_pairing.ino`:

```cpp
#define BLE_SERVICE_UUID  "12345678-1234-5678-1234-56781234abcd"
#define BLE_MAC_CHAR_UUID "12345678-1234-5678-1234-56781234abce"

// In setup(), after WiFi init:
esp_wifi_get_mac(WIFI_IF_STA, myMac);
snprintf(myMacStr, sizeof(myMacStr), "%02X:%02X:%02X:%02X:%02X:%02X",
         myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5]);

// In startBLE():
pMacCharacteristic = pBleService->createCharacteristic(
  BLE_MAC_CHAR_UUID, NIMBLE_PROPERTY::READ);
pMacCharacteristic->setValue((uint8_t*)myMacStr, 17);
```

All other behavior (ESP-NOW peer sync, GPS, heart rate, display, haptics) is unchanged from firmware 08.

## Target Selection (firmware 13)

The target-select characteristic (`abd2`) lets the iOS app tell the wearable which group member to lock onto. The wearable uses the MAC to filter inbound ESP-NOW packets and BLE scan results, and renders the name on the GC9A01 display.

**Payload:**

- **Set target:** `[6-byte MAC][1-byte name_len][name UTF-8]`. Total size must equal `7 + name_len`. `name_len` must be ≤ 24. The 6 bytes are the raw STA MAC of the target wearable in network order (same bytes that `AA:BB:CC:DD:EE:FF` encodes).
- **Clear target:** single byte `0x00`. The wearable renders a dedicated "NOT TRACKING" state and ignores all peer traffic.

**Auth:** writes are rejected unless the central has authenticated on the current connection (i.e. owner-auth on `abd0` succeeded).

**Persistence:** the target is stored in the `sw-pair` NVS namespace under keys `tmac` (6 bytes) and `tname` (string), so it survives reboots. Forgetting the owner (5-second BOOT hold) also clears the target.

**iOS behavior:**

- Group has 0 other members → write `0x00` to clear.
- Group has 1 other member → auto-write that member's MAC + display name.
- Group has 2+ other members → user picks on the Group tab; selection persists locally in `@AppStorage("trackingTarget")`.
- If the selected member has no `linked_device_mac`, surface a message in the UI and skip the BLE write.

## Peer Location (firmware 14)

The peer-location characteristic (`abd3`) streams the *currently-tracked target's* GPS to the phone so the iOS map can render the target alongside the owner's wearable.

**Payload:** identical shape to the own-location characteristic (`abd1`): `valid(u8) | lat(float32 LE) | lon(float32 LE)`, total 9 bytes. `valid=0` means one of: no target is set, no ESP-NOW packets have arrived from the target recently, or the target has no GPS fix. iOS MUST drop any existing peer pin when it sees `valid=0`.

**Cadence:** notified every `PEER_LOCATION_NOTIFY_INTERVAL_MS` (3 s). The interval timer is also force-reset on target save and target clear so the phone sees the transition within one loop iteration.

**Naming:** the wearable does not include the target's display name in this payload. iOS already knows the target's name (it wrote it via `abd2`) and labels the peer's map pin with `BLEManager.trackingTargetName`. When the target is cleared or the connection drops, `trackingTargetName` and `peerLocation` are both reset.

**iOS subscription:** `BLEManager` discovers `abd3` alongside the other service characteristics and calls `setNotifyValue(true)` during characteristic discovery. Parsed payloads populate `BLEManager.peerLocation: PeerLocation?`, which the `LocationTabView` map renders as a second (orange) annotation.

## Candidate List (firmware 16)

The candidate-list characteristic (`abd4`) lets iOS push the set of group members the wearer can choose to track from the watch itself. Selection happens on the wearable; the phone reflects the choice via the selected-target notify channel (`abd5`).

**Wire format:**

```
[u8 count]                          1 byte
repeated count times:
  [6-byte MAC]                      6 bytes
  [u8 nameLen, 1..24]               1 byte
  [nameLen bytes UTF-8 name]        ≤ 24 bytes
```

- Maximum size at `count=8`: `1 + 8 × (6 + 1 + 24) = 249` bytes — fits in the negotiated ATT MTU iOS settles on (≥ 185 bytes; ESP32 NimBLE supports up to 517).
- `count=0` (single `0x00` byte) clears the list. The firmware then auto-clears any active target that was on the list.

**Auth:** writes are rejected unless the central has authed on the current connection (owner-auth on `abd0` succeeded).

**Persistence:** the in-RAM list is mirrored to NVS under key `cands` in the `sw-pair` namespace, so it survives a reboot. On boot the firmware also validates the persisted active target (`tmac`/`tname`) against the persisted candidate list — if the target is no longer present (e.g. that user left the group between sessions), the target is cleared.

**Stale-target prune:** every successful `abd4` write triggers an "is the active target still in this list?" check. If not, the firmware:
1. Clears the active target (RAM + NVS), drops cached peer state, and forces a redraw.
2. Notifies on `abd5` with `FF×6` so iOS clears its badge immediately.

**iOS behavior:**

- On Group tab appear, on every group `add/remove/join`, and on a 20-second poll loop, iOS computes the candidate list (`g.members` filtered to non-self with `linked_device_mac`, sorted by `joined_at`, capped at 8) and writes it.
- After successful owner-auth on a reconnect, iOS automatically re-pushes the cached `lastSentCandidates` so the firmware's RAM copy stays in sync across BLE drops.

## Selected Target (firmware 16)

The selected-target characteristic (`abd5`) is the *source of truth* the iOS Group screen reads to display "Tracking <name>". It fires notifications on three events:

1. **Watch long-press confirm.** While in the NAV target-select sub-mode (see below), the wearer holds the button for 3 seconds; the firmware applies the highlighted candidate as the active target and notifies the new MAC.
2. **Phone `abd2` write.** The existing per-row "Track" button on iOS still works. After the firmware persists the selection, it mirrors the new MAC out via `abd5` so the phone (and any other subscribed central) see the change through the same channel.
3. **Stale-target prune.** When a candidate-list write removes the active target, the firmware clears it and notifies `FF×6`.

**Payload:** exactly 6 bytes — the raw target MAC, or `FF FF FF FF FF FF` to mean "no target selected".

**iOS subscription:** `BLEManager` discovers `abd5` and calls `setNotifyValue(true)` during characteristic discovery. Parsed payloads populate `BLEManager.currentTrackedMAC: String?`. The Group tab's "Tracking" header strip and the per-row badge both read this value.

## Target Select Sub-Mode (firmware 16, NAV screen)

A short tap on the NAV screen enters target-select sub-mode when the candidate list is non-empty. While in this sub-mode:

- **Quick tap (release ≤ 650 ms)** — cycle to the next candidate. The display shows the candidate's display name (large, cyan), `(X of N)` index, and "Tap to cycle / Hold 3s to confirm" hints.
- **Hold 3 s** — confirm the highlighted candidate. The firmware persists the new target, fires `abd5` notify, and shows a brief "Tracking <name>" flash before returning to normal NAV. A cyan progress arc fills around the screen perimeter while held to indicate progress.
- **8 seconds of inactivity** — exit sub-mode without changes. Active target is unchanged.
- **5 s sleep hold is gated off** while selecting — release first, then re-hold 5 s to enter Power OFF.
- **Triple-tap SOS is gated off** while selecting — three rapid taps cycle candidates rather than triggering SOS.

The first tap on entry pre-seeds the index to the currently-tracked target if it's in the list (so cycling starts from the wearer's current selection).

## GPS Sync (firmware 18)

The GPS-sync characteristic (`abd6`) is a user-initiated escape hatch for the case where the GNSS chipset can see satellites but never converges to a fix because of in-band desense from BLE/WiFi/ESP-NOW radios on the same module. Empirically, with all radios off the chipset acquires a fix in ~30 s on the same hardware that doesn't converge at all with radios up (see `arduino/gps_fix_tests/17_wearable_radios_off`).

**Trigger:** iOS writes the single byte `0x01` to `abd6`. Owner-auth gated; writes are rejected on connections that haven't completed auth via `abd0`. Other payloads are ignored.

**Response payload:** 2 bytes — `[status(u8), secondsRemaining(u8)]`.

| Status | Meaning |
|---|---|
| `0x00` | Idle — no sync recently. Initial value on a fresh connection. |
| `0x01` | Running — radios are about to come down (or are down). `secondsRemaining` is meaningful (30 on entry, decreasing). |
| `0x02` | Success — a fresh fix was acquired during the 30-second window. iOS will receive an `abd1` location notify with the new lat/lon shortly after. |
| `0x03` | Failed — 30 seconds elapsed without a fresh fix. |

**Lifecycle on the wearable:**

1. iOS write lands. Firmware sets a "trigger pending" flag and returns from the BLE callback.
2. Next loop tick: the wearable fires one `abd6` notify with status `0x01` and waits ~250 ms for it to drain over the air.
3. Wearable disconnects the central, deinits NimBLE, deinits ESP-NOW, and brings WiFi off.
4. Wearable runs a focused 30 s loop reading the GPS UART exclusively and redrawing a countdown screen on the watch face. Exits early on the first fresh fix (`gps.location.isValid() && gps.location.age() < 3000`).
5. WiFi → ESP-NOW → BLE come back up in `setup()` order. The wearable re-advertises and resumes normal operation.
6. The result (success or failed) is latched for 60 seconds. On the next owner-auth success, the firmware notifies `abd6` with the result up to five times at 500 ms intervals (BLE notifications are unreliable, especially right after re-discovery; repeating raises the chances iOS catches at least one). The location-notify cadence is also reset so a fresh fix lands on the phone within a single loop iteration.

**iOS behavior:**

- Map page shows a "Sync GPS" CTA inside the existing "Waiting for GPS fix" placeholder when the wearable is connected but `BLEManager.wearableLocation` is `nil`.
- Tapping the CTA writes `0x01` to `abd6` and immediately starts a local 30-second countdown overlay. The local countdown is the source of truth for the running UI — it keeps ticking through the BLE drop and reconnect.
- On a `status=success` notify, the overlay flashes "GPS Synced" and dismisses after 3 s.
- On a `status=failed` notify, the overlay shows "Sync Failed" with a "Try Again" button.
- A 60-second outer timeout in `BLEManager.startLocalGpsSyncCountdown()` resolves to `.failed` if no terminal status arrives — covers the case where the wearable never reconnects (e.g. it crashed, ran out of battery, or moved out of BLE range).
- The dashboard's existing reconnect loop (`DashboardView.reconnectLoop`) handles bringing the BLE link back up after the wearable's radios cycle; no special re-establish logic is needed for the sync path.
