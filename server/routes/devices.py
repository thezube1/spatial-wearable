"""/devices/link and /devices/me."""
from datetime import datetime, timezone
from flask import Blueprint, request, jsonify, g
from auth import require_auth
from db import supabase

bp = Blueprint("devices", __name__)


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


@bp.post("/devices/link")
@require_auth
def link_device():
    sb = supabase()
    body = request.get_json(silent=True) or {}
    mac = (body.get("mac") or "").strip().upper()
    if not mac:
        return jsonify({"error": "mac_required"}), 400

    existing = sb.table("devices").select("*").eq("mac_address", mac).limit(1).execute().data
    now = _now_iso()

    if existing:
        row = existing[0]
        if row.get("linked_user_id") and row["linked_user_id"] != g.user_id:
            return jsonify({"error": "already_linked"}), 409
        updated = sb.table("devices").update({
            "linked_user_id": g.user_id,
            "linked_at": now,
            "last_seen_at": now,
        }).eq("id", row["id"]).execute().data
        return jsonify({"device": updated[0]})

    inserted = sb.table("devices").insert({
        "mac_address": mac,
        "linked_user_id": g.user_id,
        "linked_at": now,
        "last_seen_at": now,
    }).execute().data
    return jsonify({"device": inserted[0]}), 201


@bp.get("/devices/me")
@require_auth
def my_device():
    sb = supabase()
    rows = sb.table("devices").select("*").eq("linked_user_id", g.user_id).limit(1).execute().data
    return jsonify({"device": rows[0] if rows else None})
