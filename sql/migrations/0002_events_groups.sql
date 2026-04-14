-- 0002_events_groups.sql
-- Events, groups, group_members + RLS.

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

create table if not exists public.group_members (
  group_id  uuid not null references public.groups(id) on delete cascade,
  user_id   uuid not null references public.users(id)  on delete cascade,
  joined_at timestamptz not null default now(),
  primary key (group_id, user_id)
);
create index if not exists group_members_user_id_idx on public.group_members(user_id);

alter table public.events        enable row level security;
alter table public.groups        enable row level security;
alter table public.group_members enable row level security;

drop policy if exists events_select_all on public.events;
create policy events_select_all on public.events
  for select using (auth.role() = 'authenticated');

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

drop policy if exists groups_insert_self on public.groups;
create policy groups_insert_self on public.groups
  for insert with check (created_by = auth.uid());

drop policy if exists groups_update_leader_or_creator on public.groups;
create policy groups_update_leader_or_creator on public.groups
  for update using (created_by = auth.uid() or leader_id = auth.uid())
           with check (created_by = auth.uid() or leader_id = auth.uid());

drop policy if exists groups_delete_creator on public.groups;
create policy groups_delete_creator on public.groups
  for delete using (created_by = auth.uid());

drop policy if exists group_members_select on public.group_members;
create policy group_members_select on public.group_members
  for select using (
    user_id = auth.uid()
    or exists (
      select 1 from public.group_members gm
      where gm.group_id = group_members.group_id and gm.user_id = auth.uid()
    )
  );

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
