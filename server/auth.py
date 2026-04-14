"""JWT verification decorator."""
from functools import wraps
import jwt
from flask import request, g, jsonify
from config import Config


def _extract_token() -> str | None:
    header = request.headers.get("Authorization", "")
    if not header.startswith("Bearer "):
        return None
    return header.split(" ", 1)[1].strip() or None


def require_auth(fn):
    @wraps(fn)
    def wrapper(*args, **kwargs):
        token = _extract_token()
        if not token:
            return jsonify({"error": "missing_bearer_token"}), 401
        try:
            payload = jwt.decode(
                token,
                Config.SUPABASE_JWT_SECRET,
                algorithms=[Config.JWT_ALGORITHM],
                audience=Config.JWT_AUDIENCE,
            )
        except jwt.ExpiredSignatureError:
            return jsonify({"error": "token_expired"}), 401
        except jwt.InvalidTokenError as e:
            return jsonify({"error": "invalid_token", "detail": str(e)}), 401

        user_id = payload.get("sub")
        if not user_id:
            return jsonify({"error": "invalid_token", "detail": "no sub"}), 401

        g.user_id = user_id
        g.jwt_payload = payload
        return fn(*args, **kwargs)

    return wrapper
