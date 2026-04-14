-- 0003_seed_events.sql
-- Seed the 4 festival events shown in the Select Event mockup (year 2026)
-- plus ~6 demo users for the Create Group member picker.

insert into public.events (name, venue, city, category, starts_on, ends_on) values
  ('Rolling Loud Orlando', 'Camping World Stadium', 'Orlando, FL',   'Hiphop Festival',       date '2026-05-08', date '2026-05-10'),
  ('Summer Smash',         'SeatGeek Stadium',      'Bridgeview, IL','Hiphop Festival',       date '2026-06-12', date '2026-06-14'),
  ('Hard Summer',          'Hollywood Park Grounds','Inglewood, CA', 'EDM + Hiphop Festival', date '2026-08-01', date '2026-08-02'),
  ('Camp Flog Gnaw',       'Dodger Stadium',        'Los Angeles, CA','EDM + Hiphop Festival',date '2026-11-22', date '2026-11-23')
on conflict do nothing;

-- Demo users for the member picker. These rows have no auth.users counterpart,
-- so we bypass the FK by seeding with service role in Supabase (which is how
-- migrations run). If the FK blocks insertion in your environment, temporarily
-- drop + recreate the FK without the reference, or create matching auth users.
-- For a demo project, we insert directly; the FK will cascade-delete if an
-- auth user with the same uuid is ever created.

-- NOTE: requires the users table FK to be deferrable OR these IDs to exist in
-- auth.users. For seed-only demo, drop the FK constraint locally if needed:
--   alter table public.users drop constraint users_id_fkey;
-- Re-add in production:
--   alter table public.users add constraint users_id_fkey
--     foreign key (id) references auth.users(id) on delete cascade;

do $$
begin
  -- Only seed if FK is absent (demo mode). Otherwise skip silently.
  if not exists (
    select 1
    from pg_constraint
    where conname = 'users_id_fkey' and conrelid = 'public.users'::regclass
  ) then
    insert into public.users (id, email, username, display_name) values
      (gen_random_uuid(), 'ally@example.com',     'taroxiao',          'Ally Wong'),
      (gen_random_uuid(), 'christine@example.com','christiny',         'Christine Lai'),
      (gen_random_uuid(), 'ashley@example.com',   'ilovemyboots',      'Ashley Chan'),
      (gen_random_uuid(), 'hudson@example.com',   'manual_driver',     'Hudson Kaneko'),
      (gen_random_uuid(), 'kyler@example.com',    'silverlaketotebag', 'Kyler Caldwell'),
      (gen_random_uuid(), 'gio@example.com',      'giogoree',          'Giovanni Goree')
    on conflict (email) do nothing;
  end if;
end $$;
