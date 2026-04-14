# iOS Onboarding — Screen-by-Screen State Machine

The flow is driven by `OnboardingCoordinator` (`@Observable`). The current
step is persisted to `UserDefaults` under key `onboardingStep`, so users
resume where they left off. The global `hasCompletedOnboarding`
(`@AppStorage`) flag is the exit condition into the dashboard.

## States

```
welcome ──▶ linkWristband ──▶ selectEvent ──▶ createOrJoin ──▶ groupSetup ──▶ confirmation ──▶ (dashboard)
```

Plus a side path from `createOrJoin` when the user chooses **Join group**
with a valid code: `createOrJoin ──▶ confirmation` (no `groupSetup`).

## Per-screen behavior

| Step | Screen | Inputs | Side effects | Next |
|------|--------|--------|--------------|------|
| `welcome` | `WelcomeView` | name, email, username, password; toggle to login-only | `supabase.auth.signUp` / `signInWithPassword` | `linkWristband` on session |
| `linkWristband` | `LinkWristbandView` | — (BLE scan auto) | `BLEManager.startScanning → connect → readMAC()`; `POST /devices/link` | `selectEvent` |
| `selectEvent` | `SelectEventView` | search query, card tap | `GET /events` | `createOrJoin` |
| `createOrJoin` | `CreateOrJoinGroupView` | Create: user search + multi-select. Join: join-code | Create: `GET /users/search?q=`. Join: `POST /groups/join` | Create→`groupSetup`, Join→`confirmation` |
| `groupSetup` | `GroupSetupView` | group name, leader avatar tap | `GET /me`, `POST /groups` | `confirmation` |
| `confirmation` | `ConfirmationView` | — | sets `hasCompletedOnboarding = true` | dashboard |

## Routing (`ContentView`)

1. `auth.currentSession == nil` → `WelcomeView`
2. Session present, `hasCompletedOnboarding == true` → `DashboardView`
3. Else → `OnboardingView` (reads `coordinator.step`). After login,
   `ContentView.task` calls `GET /devices/me`; if no linked device, the
   coordinator jumps to `.linkWristband`; otherwise the persisted step is
   resumed.

## BLE pairing protocol

- Service: `12345678-1234-5678-1234-56781234abcd`
- MAC read characteristic: `12345678-1234-5678-1234-56781234abce`
  - 17-byte ASCII string `"AA:BB:CC:DD:EE:FF"`
- Data characteristic (existing): `beb5483e-36e1-4688-b7f5-ea07361b26a8`

`BLEManager.readMAC() async throws -> String` connects → discovers →
reads the MAC characteristic and returns the string.

## Required Info.plist keys

| Key | Value | Purpose |
|-----|-------|---------|
| `SUPABASE_URL` | `https://<ref>.supabase.co` | Supabase project URL |
| `SUPABASE_ANON_KEY` | anon JWT | Supabase client auth |
| `FlaskAPIBaseURL` | e.g. `http://localhost:5050` | Backend base URL |
| `NSBluetoothAlwaysUsageDescription` | user-facing string | CoreBluetooth permission |

## Required Swift Package

Add `https://github.com/supabase/supabase-swift` (product: `Supabase`) to
the Xcode project. `Supabase/SupabaseClient.swift` and
`Supabase/AuthViewModel.swift` import `Supabase`.

## Styling notes

- Shared primitives live in `Onboarding/OnboardingStyle.swift`:
  - `OnboardingBackground` — darkened blurred-style placeholder.
  - `PrimaryBlueButtonStyle` — 337pt wide, 5pt corner radius, `Color.blue`.
  - `onboardingField()` view modifier — 337pt wide text field styling.
  - `OnboardingStyle.font(_:weight:)` — attempts `Creato Display`, falls
    back to system when the font isn't installed.
