"""Supabase client singleton (service-role)."""
from functools import lru_cache
from supabase import create_client, Client
from config import Config


@lru_cache(maxsize=1)
def supabase() -> Client:
    Config.validate()
    return create_client(Config.SUPABASE_URL, Config.SUPABASE_SERVICE_ROLE_KEY)
