"""/groups endpoints."""
from flask import Blueprint, request, jsonify, g
from auth import require_auth
from db import supabase

bp = Blueprint("groups", __name__)


def _load_group(sb, group_id: str):
    rows = sb.table("groups").select("*").eq("id", group_id).limit(1).execute().data
    return rows[0] if rows else None


def _load_members(sb, group_id: str):
    joins = sb.table("group_members").select("user_id, joined_at").eq("group_id", group_id).execute().data or []
    if not joins:
        return []
    ids = [j["user_id"] for j in joins]
    users = sb.table("users").select("id, username, display_name, avatar_url").in_("id", ids).execute().data or []
    by_id = {u["id"]: u for u in users}
    return [
        {**by_id.get(j["user_id"], {"id": j["user_id"]}), "joined_at": j["joined_at"]}
        for j in joins
    ]


@bp.post("/groups")
@require_auth
def create_group():
    sb = supabase()
    body = request.get_json(silent=True) or {}
    name = (body.get("name") or "").strip()
    event_id = body.get("event_id")
    member_ids = body.get("member_ids") or []
    leader_id = body.get("leader_id") or g.user_id

    if not name:
        return jsonify({"error": "name_required"}), 400

    all_ids = set(member_ids) | {g.user_id, leader_id}
    if leader_id not in all_ids:
        return jsonify({"error": "leader_must_be_member"}), 400

    inserted = sb.table("groups").insert({
        "name": name,
        "event_id": event_id,
        "leader_id": leader_id,
        "created_by": g.user_id,
    }).execute().data
    if not inserted:
        return jsonify({"error": "create_failed"}), 400
    group = inserted[0]

    rows = [{"group_id": group["id"], "user_id": uid} for uid in all_ids]
    sb.table("group_members").upsert(rows).execute()

    return jsonify({
        "group": group,
        "members": _load_members(sb, group["id"]),
    }), 201


@bp.get("/groups/<group_id>")
@require_auth
def get_group(group_id: str):
    sb = supabase()
    group = _load_group(sb, group_id)
    if not group:
        return jsonify({"error": "not_found"}), 404
    members = _load_members(sb, group_id)
    event = None
    if group.get("event_id"):
        ev = sb.table("events").select("*").eq("id", group["event_id"]).limit(1).execute().data
        event = ev[0] if ev else None
    leader = None
    if group.get("leader_id"):
        ld = sb.table("users").select("id, username, display_name, avatar_url").eq("id", group["leader_id"]).limit(1).execute().data
        leader = ld[0] if ld else None
    return jsonify({"group": group, "members": members, "leader": leader, "event": event})


@bp.post("/groups/<group_id>/members")
@require_auth
def add_member(group_id: str):
    sb = supabase()
    group = _load_group(sb, group_id)
    if not group:
        return jsonify({"error": "not_found"}), 404
    if g.user_id not in (group.get("created_by"), group.get("leader_id")):
        return jsonify({"error": "forbidden"}), 403
    body = request.get_json(silent=True) or {}
    user_id = body.get("user_id")
    if not user_id:
        return jsonify({"error": "user_id_required"}), 400
    sb.table("group_members").upsert({"group_id": group_id, "user_id": user_id}).execute()
    return jsonify({"members": _load_members(sb, group_id)}), 201


@bp.delete("/groups/<group_id>/members/<user_id>")
@require_auth
def remove_member(group_id: str, user_id: str):
    sb = supabase()
    group = _load_group(sb, group_id)
    if not group:
        return jsonify({"error": "not_found"}), 404
    if g.user_id not in (group.get("created_by"), group.get("leader_id")):
        return jsonify({"error": "forbidden"}), 403
    sb.table("group_members").delete().eq("group_id", group_id).eq("user_id", user_id).execute()
    return jsonify({"members": _load_members(sb, group_id)})


@bp.patch("/groups/<group_id>/leader")
@require_auth
def set_leader(group_id: str):
    sb = supabase()
    group = _load_group(sb, group_id)
    if not group:
        return jsonify({"error": "not_found"}), 404
    if g.user_id not in (group.get("created_by"), group.get("leader_id")):
        return jsonify({"error": "forbidden"}), 403
    body = request.get_json(silent=True) or {}
    new_leader = body.get("user_id")
    if not new_leader:
        return jsonify({"error": "user_id_required"}), 400

    member_rows = sb.table("group_members").select("user_id").eq("group_id", group_id).eq("user_id", new_leader).execute().data
    if not member_rows:
        return jsonify({"error": "leader_must_be_member"}), 400

    updated = sb.table("groups").update({"leader_id": new_leader}).eq("id", group_id).execute().data
    return jsonify({"group": updated[0] if updated else None})


@bp.post("/groups/join")
@require_auth
def join_by_code():
    sb = supabase()
    body = request.get_json(silent=True) or {}
    code = (body.get("join_code") or "").strip()
    if not code:
        return jsonify({"error": "join_code_required"}), 400
    rows = sb.table("groups").select("*").eq("join_code", code).limit(1).execute().data
    if not rows:
        return jsonify({"error": "not_found"}), 404
    group = rows[0]
    sb.table("group_members").upsert({"group_id": group["id"], "user_id": g.user_id}).execute()
    return jsonify({"group": group, "members": _load_members(sb, group["id"])})
