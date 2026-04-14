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
