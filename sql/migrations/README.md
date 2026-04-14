# SQL Migrations

Paste these into the Supabase SQL editor **in order**. Each file is idempotent
where practical (`create ... if not exists`, `drop policy if exists` before
recreating).

## Order

1. `0001_init_users_devices.sql` — `public.users` mirror, `handle_new_user()`
   trigger on `auth.users`, `public.devices`, RLS on both.
2. `0002_events_groups.sql` — `public.events`, `public.groups`,
   `public.group_members`, RLS + policies.
3. `0003_seed_events.sql` — inserts the 4 demo festival events and (if the
   `public.users -> auth.users` FK is dropped) the 6 demo users.

## Seeding demo users

The demo users in `0003` have no matching rows in `auth.users`. Supabase will
reject the insert because of the FK. For a demo-only project, drop that
constraint **before** running `0003`:

```sql
alter table public.users drop constraint users_id_fkey;
```

Re-add it before going to production:

```sql
alter table public.users
  add constraint users_id_fkey
  foreign key (id) references auth.users(id) on delete cascade;
```

## Full rebuild

To wipe and recreate everything from a clean slate, run `../schema.sql`
instead of the individual migrations.
