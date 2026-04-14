"""/me and /users/search endpoints."""
from flask import Blueprint, request, jsonify, g
from auth import require_auth
from db import supabase

bp = Blueprint("users", __name__)


def _current_group(user_id: str):
    sb = supabase()
    memberships = sb.table("group_members").select("group_id").eq("user_id", user_id).execute().data or []
    if not memberships:
        return None
    group_id = memberships[-1]["group_id"]
    grp = sb.table("groups").select("*").eq("id", group_id).limit(1).execute().data
    return grp[0] if grp else None


@bp.get("/me")
@require_auth
def get_me():
    sb = supabase()
    uid = g.user_id
    user = sb.table("users").select("*").eq("id", uid).limit(1).execute().data
    if not user:
        return jsonify({"error": "user_not_found"}), 404
    device = sb.table("devices").select("*").eq("linked_user_id", uid).limit(1).execute().data
    return jsonify({
        "user": user[0],
        "device": device[0] if device else None,
        "group": _current_group(uid),
    })


@bp.patch("/me")
@require_auth
def patch_me():
    sb = supabase()
    body = request.get_json(silent=True) or {}
    updates = {k: v for k, v in body.items() if k in ("username", "display_name", "avatar_url")}
    if not updates:
        return jsonify({"error": "no_fields"}), 400
    res = sb.table("users").update(updates).eq("id", g.user_id).execute()
    if not res.data:
        return jsonify({"error": "update_failed"}), 400
    return jsonify({"user": res.data[0]})


@bp.get("/users/search")
@require_auth
def search_users():
    sb = supabase()
    q = (request.args.get("q") or "").strip()
    query = sb.table("users").select("id, username, display_name, avatar_url").neq("id", g.user_id)
    if q:
        pattern = f"%{q}%"
        query = query.or_(f"username.ilike.{pattern},display_name.ilike.{pattern}")
    rows = query.limit(20).execute().data or []
    return jsonify({"users": rows})
