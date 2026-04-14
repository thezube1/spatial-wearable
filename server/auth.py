"""JWT verification decorator.

Supports Supabase's asymmetric JWT signing (RS256/ES256 via JWKS) with
fallback to legacy HS256 shared-secret verification.
"""
import logging
from functools import wraps

import jwt
from flask import g, jsonify, request

from config import Config

logger = logging.getLogger(__name__)

_jwks_client: jwt.PyJWKClient | None = None


def _get_jwks_client() -> jwt.PyJWKClient | None:
    global _jwks_client
    if _jwks_client is None and Config.SUPABASE_JWKS_URL:
        _jwks_client = jwt.PyJWKClient(Config.SUPABASE_JWKS_URL, cache_keys=True)
    return _jwks_client


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
            alg = jwt.get_unverified_header(token).get("alg")
        except jwt.InvalidTokenError as e:
            return jsonify({"error": "invalid_token", "detail": str(e)}), 401

        payload = None
        last_error: Exception | None = None

        jwks_client = _get_jwks_client()
        if jwks_client is not None:
            try:
                signing_key = jwks_client.get_signing_key_from_jwt(token)
                payload = jwt.decode(
                    token,
                    signing_key.key,
                    algorithms=["RS256", "ES256"],
                    audience=Config.JWT_AUDIENCE,
                )
            except jwt.ExpiredSignatureError:
                return jsonify({"error": "token_expired"}), 401
            except Exception as e:
                logger.info(
                    "JWKS verification failed (alg=%s, %s: %s); trying HS256 fallback",
                    alg, type(e).__name__, e,
                )
                last_error = e

        if payload is None and Config.SUPABASE_JWT_SECRET:
            try:
                payload = jwt.decode(
                    token,
                    Config.SUPABASE_JWT_SECRET,
                    algorithms=["HS256"],
                    audience=Config.JWT_AUDIENCE,
                )
            except jwt.ExpiredSignatureError:
                return jsonify({"error": "token_expired"}), 401
            except jwt.InvalidTokenError as e:
                last_error = e

        if payload is None:
            detail = str(last_error) if last_error else "verification failed"
            return jsonify({"error": "invalid_token", "detail": detail}), 401

        user_id = payload.get("sub")
        if not user_id:
            return jsonify({"error": "invalid_token", "detail": "no sub"}), 401

        g.user_id = user_id
        g.jwt_payload = payload
        return fn(*args, **kwargs)

    return wrapper
