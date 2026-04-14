"""Flask app factory for the Spatial Wearable API."""
from flask import Flask, jsonify
from flask_cors import CORS

from routes.users import bp as users_bp
from routes.devices import bp as devices_bp
from routes.events import bp as events_bp
from routes.groups import bp as groups_bp


def create_app() -> Flask:
    app = Flask(__name__)
    CORS(app)

    app.register_blueprint(users_bp)
    app.register_blueprint(devices_bp)
    app.register_blueprint(events_bp)
    app.register_blueprint(groups_bp)

    @app.get("/health")
    def health():
        return jsonify({"ok": True})

    return app


app = create_app()


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=True)
