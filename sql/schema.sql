-- Spatial Wearable — full schema (source of truth)
-- Run on a fresh Supabase project. Idempotent where practical.

create extension if not exists pgcrypto;

-- =========================================================================
-- public.users (mirror of auth.users)
-- =========================================================================
create table if not exists public.users (
  id           uuid primary key references auth.users(id) on delete cascade,
  email        text not null unique,
  username     text unique,
  display_name text,
  avatar_url   text,
  created_at   timestamptz not null default now()
);

-- Trigger function: mirror new auth.users rows into public.users.
create or replace function public.handle_new_user()
returns trigger
language plpgsql
security definer
set search_path = public
as $$
begin
  insert into public.users (id, email, username, display_name, avatar_url)
  values (
    new.id,
    new.email,
    coalesce(new.raw_user_meta_data->>'username', null),
    coalesce(new.raw_user_meta_data->>'display_name', new.raw_user_meta_data->>'name', null),
    coalesce(new.raw_user_meta_data->>'avatar_url', null)
  )
  on conflict (id) do nothing;
  return new;
end;
$$;

drop trigger if exists on_auth_user_created on auth.users;
create trigger on_auth_user_created
  after insert on auth.users
  for each row execute function public.handle_new_user();

-- =========================================================================
-- public.devices
-- =========================================================================
create table if not exists public.devices (
  id             uuid primary key default gen_random_uuid(),
  mac_address    text not null unique,
  linked_user_id uuid references public.users(id) on delete set null,
  linked_at      timestamptz,
  last_seen_at   timestamptz,
  created_at     timestamptz not null default now()
);
create index if not exists devices_linked_user_id_idx on public.devices(linked_user_id);

-- =========================================================================
-- public.events
-- =========================================================================
create table if not exists public.events (
  id        uuid primary key default gen_random_uuid(),
  name      text not null,
  venue     text,
  city      text,
  category  text,
  starts_on date,
  ends_on   date,
  image_url text
);

-- =========================================================================
-- public.groups
-- =========================================================================
create table if not exists public.groups (
  id         uuid primary key default gen_random_uuid(),
  name       text not null,
  event_id   uuid references public.events(id),
  leader_id  uuid references public.users(id),
  created_by uuid not null references public.users(id),
  join_code  text unique default substr(md5(random()::text), 1, 6),
  created_at timestamptz not null default now()
);
create index if not exists groups_event_id_idx  on public.groups(event_id);
create index if not exists groups_leader_id_idx on public.groups(leader_id);

-- =========================================================================
-- public.group_members
-- =========================================================================
create table if not exists public.group_members (
  group_id  uuid not null references public.groups(id) on delete cascade,
  user_id   uuid not null references public.users(id)  on delete cascade,
  joined_at timestamptz not null default now(),
  primary key (group_id, user_id)
);
create index if not exists group_members_user_id_idx on public.group_members(user_id);

-- =========================================================================
-- Row Level Security
-- =========================================================================
alter table public.users         enable row level security;
alter table public.devices       enable row level security;
alter table public.events        enable row level security;
alter table public.groups        enable row level security;
alter table public.group_members enable row level security;

-- users: read your own row; update your own row. Service role bypasses.
drop policy if exists users_select_self on public.users;
create policy users_select_self on public.users
  for select using (auth.uid() = id);

drop policy if exists users_update_self on public.users;
create policy users_update_self on public.users
  for update using (auth.uid() = id) with check (auth.uid() = id);

-- devices: a user can read their own linked device.
drop policy if exists devices_select_own on public.devices;
create policy devices_select_own on public.devices
  for select using (linked_user_id = auth.uid());

-- events: public read (authenticated users).
drop policy if exists events_select_all on public.events;
create policy events_select_all on public.events
  for select using (auth.role() = 'authenticated');

-- groups: a user can read groups they belong to or created.
drop policy if exists groups_select_member on public.groups;
create policy groups_select_member on public.groups
  for select using (
    created_by = auth.uid()
    or leader_id = auth.uid()
    or exists (
      select 1 from public.group_members gm
      where gm.group_id = id and gm.user_id = auth.uid()
    )
  );

-- groups: creator or current leader can update; creator can delete.
drop policy if exists groups_update_leader_or_creator on public.groups;
create policy groups_update_leader_or_creator on public.groups
  for update using (created_by = auth.uid() or leader_id = auth.uid())
           with check (created_by = auth.uid() or leader_id = auth.uid());

drop policy if exists groups_delete_creator on public.groups;
create policy groups_delete_creator on public.groups
  for delete using (created_by = auth.uid());

drop policy if exists groups_insert_self on public.groups;
create policy groups_insert_self on public.groups
  for insert with check (created_by = auth.uid());

-- group_members: members can read their groups' rosters.
drop policy if exists group_members_select on public.group_members;
create policy group_members_select on public.group_members
  for select using (
    user_id = auth.uid()
    or exists (
      select 1 from public.group_members gm
      where gm.group_id = group_members.group_id and gm.user_id = auth.uid()
    )
  );

-- group_members: creator or leader may add/remove members.
drop policy if exists group_members_mutate on public.group_members;
create policy group_members_mutate on public.group_members
  for all using (
    exists (
      select 1 from public.groups g
      where g.id = group_members.group_id
        and (g.created_by = auth.uid() or g.leader_id = auth.uid())
    )
  ) with check (
    exists (
      select 1 from public.groups g
      where g.id = group_members.group_id
        and (g.created_by = auth.uid() or g.leader_id = auth.uid())
    )
  );
