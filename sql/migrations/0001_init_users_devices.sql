-- 0001_init_users_devices.sql
-- Creates public.users (mirror of auth.users), handle_new_user trigger, and public.devices.

create extension if not exists pgcrypto;

create table if not exists public.users (
  id           uuid primary key references auth.users(id) on delete cascade,
  email        text not null unique,
  username     text unique,
  display_name text,
  avatar_url   text,
  created_at   timestamptz not null default now()
);

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

create table if not exists public.devices (
  id             uuid primary key default gen_random_uuid(),
  mac_address    text not null unique,
  linked_user_id uuid references public.users(id) on delete set null,
  linked_at      timestamptz,
  last_seen_at   timestamptz,
  created_at     timestamptz not null default now()
);
create index if not exists devices_linked_user_id_idx on public.devices(linked_user_id);

alter table public.users   enable row level security;
alter table public.devices enable row level security;

drop policy if exists users_select_self on public.users;
create policy users_select_self on public.users
  for select using (auth.uid() = id);

drop policy if exists users_update_self on public.users;
create policy users_update_self on public.users
  for update using (auth.uid() = id) with check (auth.uid() = id);

drop policy if exists devices_select_own on public.devices;
create policy devices_select_own on public.devices
  for select using (linked_user_id = auth.uid());
