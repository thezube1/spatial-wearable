# Supabase Setup

## 1. Create a project

1. Go to <https://supabase.com> and create a new project.
2. Pick a region, set a strong DB password, wait for provisioning.

## 2. Grab the three values the Flask server needs

Project settings → **API**:

| Env var | Where |
|---|---|
| `SUPABASE_URL` | "Project URL" |
| `SUPABASE_SERVICE_ROLE_KEY` | "Project API keys" → `service_role` (secret — server only) |
| `SUPABASE_JWT_SECRET` | "JWT Settings" → "JWT Secret" |

The iOS app uses the `anon` key and the same project URL.

## 3. Run migrations (SQL editor, in order)

Open **SQL Editor** in the Supabase dashboard and paste each file in turn:

1. `sql/migrations/0001_init_users_devices.sql`
2. `sql/migrations/0002_events_groups.sql`
3. `sql/migrations/0003_seed_events.sql`

Before running `0003`, if you want the 6 demo users to seed successfully,
temporarily drop the FK from `public.users.id → auth.users.id`:

```sql
alter table public.users drop constraint users_id_fkey;
```

After demo, restore it:

```sql
alter table public.users
  add constraint users_id_fkey
  foreign key (id) references auth.users(id) on delete cascade;
```

Alternatively, wipe and apply `sql/schema.sql` which is a single-shot full
build of everything except the demo-user seeds.

## 4. Verify

```sql
select count(*) from public.events;        -- expect 4
select count(*) from public.users;         -- expect 6 if demo seeds ran
select tgname from pg_trigger where tgname = 'on_auth_user_created';
```

Sign up a test user via the Supabase dashboard (Authentication → Users → Add
user). A matching row should appear in `public.users`.

## 5. Point the Flask server at the project

```bash
cd server
cp .env.example .env
# paste SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY, SUPABASE_JWT_SECRET
pip install -r requirements.txt
flask --app app run
```

Mint a JWT for testing from the Supabase dashboard (or sign in from the iOS
app and log the `access_token`). Hit `GET /events` with
`Authorization: Bearer <jwt>` and you should see the 4 seeded festivals.
