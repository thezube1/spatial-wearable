# API Reference

Base URL: `http://localhost:5050` (dev).

All endpoints except `/health` require:

```
Authorization: Bearer <supabase_jwt>
```

JWTs are issued by Supabase Auth (HS256, audience `authenticated`). The server
verifies them with `SUPABASE_JWT_SECRET` and extracts `sub` as the user id.

## Errors

JSON body: `{"error": "<code>", "detail": "<optional>"}`.

| Status | Meaning |
|---|---|
| 400 | Bad request / validation |
| 401 | Missing or invalid token |
| 403 | Authenticated but not allowed |
| 404 | Not found |
| 409 | Conflict (e.g. device linked to another user) |

---

## Health

### GET /health
No auth. Returns `{"ok": true}`.

---

## Users

### GET /me
Returns the current user's profile plus their linked device (if any) and
current group (if any).

Response 200:
```json
{
  "user":   { "id": "...", "email": "...", "username": "...", "display_name": "...", "avatar_url": null, "created_at": "..." },
  "device": { "id": "...", "mac_address": "AA:BB:CC:DD:EE:FF", "linked_user_id": "...", "linked_at": "...", "last_seen_at": "...", "created_at": "..." },
  "group":  { "id": "...", "name": "...", "event_id": "...", "leader_id": "...", "created_by": "...", "join_code": "abc123", "created_at": "..." }
}
```

### PATCH /me
Body: any subset of `{"username", "display_name", "avatar_url"}`.
Response 200: `{"user": {...}}`. 400 if no valid fields.

### GET /users/search?q=<string>
Case-insensitive `ILIKE` match against `username` and `display_name`. Empty
`q` returns the first 20 users (for the Create Group member picker).

Response 200: `{"users": [{"id", "username", "display_name", "avatar_url"}, ...]}`.

---

## Devices

### POST /devices/link
Body: `{"mac": "AA:BB:CC:DD:EE:FF"}`.

- Creates a `devices` row if none exists for that MAC.
- If one exists and is unlinked or linked to this user, updates
  `linked_user_id` + timestamps.
- If linked to a different user: `409 already_linked`.

Response 200/201: `{"device": {...}}`.

### GET /devices/me
Response 200: `{"device": {...} | null}`.

---

## Events

### GET /events
Returns events with `ends_on >= today`, ordered by `starts_on` ascending.
Response 200: `{"events": [...]}`.

### GET /events/:id
Response 200: `{"event": {...}}`. 404 if missing.

---

## Groups

### POST /groups
Body:
```json
{
  "name": "string",
  "event_id": "uuid | null",
  "member_ids": ["uuid", ...],
  "leader_id": "uuid (optional — defaults to creator)"
}
```

Creates the group, inserts the creator + `member_ids` + `leader_id` into
`group_members`, sets `leader_id` (default = creator).

Response 201: `{"group": {...}, "members": [...]}`.
400 if `name` missing or `leader_id` not in member set.

### GET /groups/:id
Response 200: `{"group", "members", "leader", "event"}`. 404 if missing.

### POST /groups/:id/members
Body: `{"user_id": "uuid"}`. Creator or current leader only.
Response 201: `{"members": [...]}`. 403 otherwise.

### DELETE /groups/:id/members/:user_id
Creator or current leader only.
Response 200: `{"members": [...]}`.

### PATCH /groups/:id/leader
Body: `{"user_id": "uuid"}`. Target user must be a member. Creator or current
leader only.
Response 200: `{"group": {...}}`. 400 if not a member. 403 otherwise.

### POST /groups/join
Body: `{"join_code": "abc123"}`.
Adds the current user to the matching group.
Response 200: `{"group", "members"}`. 404 if code unknown.
