"""/events endpoints."""
from datetime import date
from flask import Blueprint, jsonify
from auth import require_auth
from db import supabase

bp = Blueprint("events", __name__)


@bp.get("/events")
@require_auth
def list_events():
    sb = supabase()
    today = date.today().isoformat()
    rows = (
        sb.table("events")
        .select("*")
        .gte("ends_on", today)
        .order("starts_on", desc=False)
        .execute()
        .data
        or []
    )
    return jsonify({"events": rows})


@bp.get("/events/<event_id>")
@require_auth
def get_event(event_id: str):
    sb = supabase()
    rows = sb.table("events").select("*").eq("id", event_id).limit(1).execute().data
    if not rows:
        return jsonify({"error": "not_found"}), 404
    return jsonify({"event": rows[0]})
