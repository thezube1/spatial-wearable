"""Environment configuration."""
import os
from dotenv import load_dotenv

load_dotenv()


class Config:
    SUPABASE_URL = os.environ.get("SUPABASE_URL", "")
    SUPABASE_SERVICE_ROLE_KEY = os.environ.get("SUPABASE_SERVICE_ROLE_KEY", "")
    SUPABASE_JWT_SECRET = os.environ.get("SUPABASE_JWT_SECRET", "")
    JWT_AUDIENCE = "authenticated"
    JWT_ALGORITHM = "HS256"

    @classmethod
    def validate(cls) -> None:
        missing = [
            name for name in ("SUPABASE_URL", "SUPABASE_SERVICE_ROLE_KEY", "SUPABASE_JWT_SECRET")
            if not getattr(cls, name)
        ]
        if missing:
            raise RuntimeError(f"Missing env vars: {', '.join(missing)}")
