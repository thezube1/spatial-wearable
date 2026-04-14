# Architecture

```
iOS (SwiftUI + supabase-swift)
  ├── Auth: Supabase direct (email/password)
  ├── Data reads/writes: Flask API (Bearer <supabase JWT>)
  └── Wearable pairing: CoreBluetooth -> reads MAC characteristic
        |
        v
Flask API (Python 3.11)
  ├── JWT verification via Supabase JWT secret
  ├── Uses supabase-py with service-role key for privileged ops
  └── Endpoints: /me, /devices/link, /events, /users/search, /groups/*
        |
        v
Supabase (Postgres + Auth)
  ├── auth.users (managed by Supabase)
  ├── public.users (mirror, trigger on auth.users insert)
  ├── public.devices (mac unique, linked_user_id nullable FK)
  ├── public.events (seeded)
  ├── public.groups (leader_id, event_id, created_by)
  └── public.group_members (group_id, user_id) - PK composite
```

## Data flow

1. **Sign up / sign in.** The iOS app talks to Supabase Auth directly with the
   `anon` key. On success, Supabase returns a JWT (HS256, audience
   `authenticated`). A Postgres trigger `on_auth_user_created` mirrors the new
   `auth.users` row into `public.users` so the app has a profile record.

2. **Authenticated API calls.** Every call from iOS to the Flask API carries
   `Authorization: Bearer <jwt>`. The `@require_auth` decorator verifies the
   signature with `SUPABASE_JWT_SECRET`, checks audience, and puts the user id
   on `flask.g.user_id`.

3. **Privileged DB access.** The Flask server uses `supabase-py` configured
   with the `service_role` key, which bypasses RLS. All cross-user reads
   (member rosters, user search, group creation on behalf of a user) go
   through the server so RLS on the database is still a meaningful defense if
   a client ever tries to hit Supabase directly with an `anon` key.

4. **Wearable pairing.** During onboarding the iOS app connects to the
   wearable over BLE, reads a MAC-address characteristic, and POSTs it to
   `/devices/link`. The server upserts a `public.devices` row with
   `linked_user_id = g.user_id`. Rejects the link with 409 if the MAC is
   already bound to another user.

5. **Event + group selection.** The iOS onboarding coordinator drives
   `/events`, `/users/search`, `/groups` in sequence. The group creator is
   added to `group_members` automatically and becomes the default leader;
   leader can later be reassigned through `PATCH /groups/:id/leader`.

6. **Dashboard.** Post-onboarding the app calls `/me` and renders the event,
   group, and device cards from the single response.

## Key decisions

- iOS talks to Supabase Auth directly (fewer round trips; Supabase SDK handles
  token refresh). Flask only handles business-logic endpoints.
- Service-role writes on the server side keep RLS as defense-in-depth without
  forcing every query to thread the user's JWT through PostgREST.
- Device MAC is the stable identity for a wearable; `devices.linked_user_id`
  can be nullified on unlink without destroying history.
