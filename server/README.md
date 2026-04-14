# Spatial Wearable — Flask API

Intermediary between the iOS app and Supabase. Verifies Supabase JWTs (HS256,
audience `authenticated`) and uses the service-role key for privileged writes.

## Setup

```bash
cd server
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
cp .env.example .env   # then fill in the three SUPABASE_* values
```

## Run

```bash
flask --app app run --debug
# or: python app.py
```

### Docker Compose

From the repo root:

```bash
cp server/.env.example server/.env   # fill in SUPABASE_* values
docker compose up --build
```

The `server/` directory is bind-mounted for live reload in debug mode. For a
production-style run (gunicorn, no reload), remove the `command:` and `volumes:`
keys in `docker-compose.yml` or build + run the image directly:

```bash
docker build -t spatial-wearable-api ./server
docker run --rm -p 5000:5000 --env-file server/.env spatial-wearable-api
```

Server listens on `http://localhost:5000`. Every request (except `/health`)
requires `Authorization: Bearer <supabase-jwt>`.

## Environment variables

| Name | Source |
|---|---|
| `SUPABASE_URL` | Supabase project settings → API → Project URL |
| `SUPABASE_SERVICE_ROLE_KEY` | Project settings → API → service_role key |
| `SUPABASE_JWT_SECRET` | Project settings → API → JWT Settings → JWT Secret |

See `../documentation/api.md` for the full endpoint reference.
