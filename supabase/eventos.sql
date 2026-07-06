-- ============================================================
-- SUPREME FAN IOT - SUPABASE TABLE
-- Run this script in Supabase:
-- SQL Editor -> New query -> Run
--
-- This migration updates the previous potentiometer/PIR table
-- without deleting the table or its existing rows.
-- ============================================================

create table if not exists public.eventos (
    id bigint generated always as identity primary key,
    created_at timestamptz not null default now()
);

-- Telemetry sent by the ESP32 gateway to Flask.
alter table public.eventos
    add column if not exists sequence bigint,
    add column if not exists uptime_ms bigint,
    add column if not exists temperature_c numeric(6, 2),
    add column if not exists temperature_f numeric(6, 2),
    add column if not exists humidity numeric(6, 2),
    add column if not exists distance_cm integer,
    add column if not exists fan_on boolean not null default false,
    add column if not exists manual_mode boolean not null default false,
    add column if not exists sensors_valid boolean not null default false,
    add column if not exists alert_active boolean not null default false,
    add column if not exists rssi smallint;

-- Remove fields that belonged to the previous potentiometer/PIR project.
alter table public.eventos
    drop column if exists pot_valor,
    drop column if exists pot_porcentaje,
    drop column if exists movimiento,
    drop column if exists led_alerta,
    drop column if exists comando_manual;

-- Useful indexes for the dashboard history.
create index if not exists eventos_created_at_idx
    on public.eventos (created_at desc);

create index if not exists eventos_sequence_idx
    on public.eventos (sequence);

-- Enable Row Level Security.
alter table public.eventos enable row level security;

-- Remove previous policies so the script can be executed more than once.
drop policy if exists "Permitir insercion publica"
    on public.eventos;

drop policy if exists "Permitir lectura publica"
    on public.eventos;

drop policy if exists "Allow anonymous inserts"
    on public.eventos;

drop policy if exists "Allow anonymous reads"
    on public.eventos;

-- Classroom/demo policies.
-- These allow Flask to use the Supabase anon key for insert and select.
create policy "Allow anonymous inserts"
on public.eventos
for insert
to anon
with check (true);

create policy "Allow anonymous reads"
on public.eventos
for select
to anon
using (true);

-- Explicit API permissions for anon and authenticated roles.
grant usage on schema public to anon, authenticated;
grant select, insert on table public.eventos to anon, authenticated;

-- Identity sequence permissions, when the sequence exists.
do $$
begin
    if to_regclass('public.eventos_id_seq') is not null then
        execute
            'grant usage, select on sequence public.eventos_id_seq '
            'to anon, authenticated';
    end if;
end
$$;

-- Optional verification query.
select
    id,
    created_at,
    sequence,
    temperature_c,
    humidity,
    distance_cm,
    fan_on,
    manual_mode,
    alert_active,
    rssi
from public.eventos
order by created_at desc
limit 10;
