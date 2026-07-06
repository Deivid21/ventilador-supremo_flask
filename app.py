"""
Flask backend for the Supreme Fan IoT project.

Routes:
    GET  /              Dashboard
    POST /comando       Receive commands from the HTML form
    POST /api/datos     Receive telemetry from the ESP32 gateway
    GET  /api/comando   Return the latest command to the ESP32 gateway
    GET  /api/estado    Return the latest telemetry as JSON
    GET  /health        Basic service status

Supabase is optional while developing locally. When SUPABASE_URL and
SUPABASE_KEY are configured, telemetry is also stored in the "eventos" table.
"""

from __future__ import annotations

import json
import logging
import math
import os
import re
import threading
import time
from collections import deque
from datetime import datetime, timezone
from typing import Any

from dotenv import load_dotenv
from flask import Flask, jsonify, redirect, render_template, request, url_for

try:
    from supabase import Client, create_client
except ImportError:
    Client = Any  # type: ignore[misc,assignment]
    create_client = None


load_dotenv()

app = Flask(__name__)
app.config["JSON_SORT_KEYS"] = False
app.config["MAX_CONTENT_LENGTH"] = 16 * 1024

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
logger = logging.getLogger(__name__)


# -----------------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------------
SUPABASE_URL = os.getenv("SUPABASE_URL", "").strip()
SUPABASE_KEY = os.getenv("SUPABASE_KEY", "").strip()
SUPABASE_TABLE = os.getenv("SUPABASE_TABLE", "eventos").strip()

HISTORY_LIMIT = 30
DEVICE_TIMEOUT_SECONDS = 7.0

TELEMETRY_FIELDS = (
    "sequence",
    "uptime_ms",
    "temperature_c",
    "temperature_f",
    "humidity",
    "distance_cm",
    "fan_on",
    "manual_mode",
    "sensors_valid",
    "alert_active",
    "rssi",
)

VALID_COMMANDS = {
    "AUTO",
    "MANUAL",
    "FAN_ON",
    "FAN_OFF",
}

FORM_ACTIONS = {
    "auto": "AUTO",
    "automatic": "AUTO",
    "manual": "MANUAL",
    "fan_on": "FAN_ON",
    "on": "FAN_ON",
    "activar": "FAN_ON",
    "fan_off": "FAN_OFF",
    "off": "FAN_OFF",
    "desactivar": "FAN_OFF",
}


# -----------------------------------------------------------------------------
# Runtime state
# -----------------------------------------------------------------------------
state_lock = threading.Lock()

latest_state: dict[str, Any] = {
    "sequence": None,
    "uptime_ms": None,
    "temperature_c": None,
    "temperature_f": None,
    "humidity": None,
    "distance_cm": None,
    "fan_on": False,
    "manual_mode": False,
    "sensors_valid": False,
    "alert_active": False,
    "rssi": None,
    "last_update": None,
    "last_update_epoch": None,
}

latest_command: dict[str, Any] = {
    "command": "NONE",
    "command_id": 0,
    "updated_at": None,
}

local_history: deque[dict[str, Any]] = deque(maxlen=HISTORY_LIMIT)


# -----------------------------------------------------------------------------
# Supabase initialization
# -----------------------------------------------------------------------------
supabase: Client | None = None

if SUPABASE_URL and SUPABASE_KEY:
    if create_client is None:
        logger.warning(
            "Supabase credentials were found, but the 'supabase' package "
            "is not installed. Local storage will be used."
        )
    else:
        try:
            supabase = create_client(SUPABASE_URL, SUPABASE_KEY)
            logger.info("Supabase client initialized.")
        except Exception as error:
            logger.exception("Could not initialize Supabase: %s", error)
else:
    logger.warning(
        "SUPABASE_URL or SUPABASE_KEY is missing. "
        "The backend will run with temporary in-memory history."
    )


# -----------------------------------------------------------------------------
# Utility functions
# -----------------------------------------------------------------------------
def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def utc_now_iso() -> str:
    return utc_now().isoformat(timespec="seconds")


def parse_json_request() -> dict[str, Any] | None:
    """
    Parse a JSON request safely.

    The fallback also converts non-standard values such as lowercase "nan",
    which can occasionally be produced by embedded devices, into JSON null.
    """
    payload = request.get_json(silent=True)

    if isinstance(payload, dict):
        return payload

    raw_body = request.get_data(as_text=True).strip()
    if not raw_body:
        return None

    normalized_body = re.sub(
        r"\b(?:nan|inf|-inf|infinity|-infinity)\b",
        "null",
        raw_body,
        flags=re.IGNORECASE,
    )

    try:
        decoded = json.loads(normalized_body)
    except json.JSONDecodeError:
        return None

    return decoded if isinstance(decoded, dict) else None


def require_integer(
    payload: dict[str, Any],
    key: str,
    minimum: int | None = None,
    maximum: int | None = None,
) -> int:
    value = payload.get(key)

    if isinstance(value, bool):
        raise ValueError(f"'{key}' must be an integer.")

    try:
        converted = int(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f"'{key}' must be an integer.") from error

    if minimum is not None and converted < minimum:
        raise ValueError(f"'{key}' must be greater than or equal to {minimum}.")

    if maximum is not None and converted > maximum:
        raise ValueError(f"'{key}' must be less than or equal to {maximum}.")

    return converted


def optional_float(payload: dict[str, Any], key: str) -> float | None:
    value = payload.get(key)

    if value is None:
        return None

    if isinstance(value, bool):
        raise ValueError(f"'{key}' must be a number or null.")

    try:
        converted = float(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f"'{key}' must be a number or null.") from error

    if not math.isfinite(converted):
        return None

    return round(converted, 2)


def require_boolean(payload: dict[str, Any], key: str) -> bool:
    value = payload.get(key)

    if isinstance(value, bool):
        return value

    if value in (0, 1):
        return bool(value)

    if isinstance(value, str):
        normalized = value.strip().lower()
        if normalized in {"true", "1", "on", "yes"}:
            return True
        if normalized in {"false", "0", "off", "no"}:
            return False

    raise ValueError(f"'{key}' must be a boolean.")


def normalize_telemetry(payload: dict[str, Any]) -> dict[str, Any]:
    missing_fields = [field for field in TELEMETRY_FIELDS if field not in payload]
    if missing_fields:
        raise ValueError(
            "Missing telemetry fields: " + ", ".join(missing_fields)
        )

    telemetry = {
        "sequence": require_integer(payload, "sequence", minimum=0),
        "uptime_ms": require_integer(payload, "uptime_ms", minimum=0),
        "temperature_c": optional_float(payload, "temperature_c"),
        "temperature_f": optional_float(payload, "temperature_f"),
        "humidity": optional_float(payload, "humidity"),
        "distance_cm": require_integer(
            payload,
            "distance_cm",
            minimum=-1,
            maximum=10000,
        ),
        "fan_on": require_boolean(payload, "fan_on"),
        "manual_mode": require_boolean(payload, "manual_mode"),
        "sensors_valid": require_boolean(payload, "sensors_valid"),
        "alert_active": require_boolean(payload, "alert_active"),
        "rssi": require_integer(payload, "rssi", minimum=-127, maximum=20),
    }

    if telemetry["humidity"] is not None:
        if not 0 <= telemetry["humidity"] <= 100:
            raise ValueError("'humidity' must be between 0 and 100.")

    if telemetry["sensors_valid"]:
        if telemetry["temperature_c"] is None:
            raise ValueError(
                "'temperature_c' cannot be null when sensors_valid is true."
            )
        if telemetry["humidity"] is None:
            raise ValueError(
                "'humidity' cannot be null when sensors_valid is true."
            )
        if telemetry["distance_cm"] < 0:
            raise ValueError(
                "'distance_cm' cannot be negative when sensors_valid is true."
            )

    return telemetry


def update_runtime_state(telemetry: dict[str, Any]) -> dict[str, Any]:
    received_at = utc_now_iso()
    event = {
        **telemetry,
        "created_at": received_at,
    }

    with state_lock:
        latest_state.update(telemetry)
        latest_state["last_update"] = received_at
        latest_state["last_update_epoch"] = time.time()
        local_history.appendleft(event)

    return event


def save_event_to_supabase(telemetry: dict[str, Any]) -> bool:
    if supabase is None:
        return False

    try:
        supabase.table(SUPABASE_TABLE).insert(telemetry).execute()
        return True
    except Exception as error:
        logger.exception("Could not save telemetry in Supabase: %s", error)
        return False


def fetch_history() -> list[dict[str, Any]]:
    if supabase is not None:
        try:
            response = (
                supabase.table(SUPABASE_TABLE)
                .select("*")
                .order("created_at", desc=True)
                .limit(HISTORY_LIMIT)
                .execute()
            )
            if response.data:
                return list(response.data)
        except Exception as error:
            logger.exception("Could not read history from Supabase: %s", error)

    with state_lock:
        return list(local_history)


def get_state_snapshot() -> dict[str, Any]:
    with state_lock:
        snapshot = dict(latest_state)

    last_update_epoch = snapshot.pop("last_update_epoch", None)

    if last_update_epoch is None:
        snapshot["online"] = False
        snapshot["seconds_since_update"] = None
    else:
        elapsed = max(0.0, time.time() - float(last_update_epoch))
        snapshot["online"] = elapsed <= DEVICE_TIMEOUT_SECONDS
        snapshot["seconds_since_update"] = round(elapsed, 1)

    return snapshot


def get_command_snapshot() -> dict[str, Any]:
    with state_lock:
        return dict(latest_command)


def register_command(command: str) -> dict[str, Any]:
    normalized_command = command.strip().upper()

    if normalized_command not in VALID_COMMANDS:
        raise ValueError(f"Unsupported command: {normalized_command}")

    with state_lock:
        latest_command["command_id"] += 1
        latest_command["command"] = normalized_command
        latest_command["updated_at"] = utc_now_iso()
        return dict(latest_command)


# -----------------------------------------------------------------------------
# Dashboard routes
# -----------------------------------------------------------------------------
@app.get("/")
def dashboard():
    state = get_state_snapshot()
    command = get_command_snapshot()
    history = fetch_history()

    return render_template(
        "dashboard.html",
        state=state,
        command=command,
        history=history,
        # Temporary aliases keep the previous dashboard template loadable
        # until it is replaced in the next project step.
        estado=state,
        comando=command,
        historial=history,
    )


@app.post("/comando")
def dashboard_command():
    action = request.form.get("accion", "").strip().lower()
    command = FORM_ACTIONS.get(action)

    if command is None:
        logger.warning("Unknown dashboard action: %s", action)
        return redirect(url_for("dashboard"))

    registered = register_command(command)
    logger.info(
        "Dashboard command registered: %s (ID %s)",
        registered["command"],
        registered["command_id"],
    )

    return redirect(url_for("dashboard"))


# -----------------------------------------------------------------------------
# ESP32 API
# -----------------------------------------------------------------------------
@app.post("/api/datos")
def receive_telemetry():
    payload = parse_json_request()

    if payload is None:
        return jsonify(
            {
                "ok": False,
                "error": "The request body must contain a valid JSON object.",
            }
        ), 400

    try:
        telemetry = normalize_telemetry(payload)
    except ValueError as error:
        logger.warning("Rejected telemetry: %s", error)
        return jsonify(
            {
                "ok": False,
                "error": str(error),
            }
        ), 400

    event = update_runtime_state(telemetry)
    saved_to_supabase = save_event_to_supabase(telemetry)

    logger.info(
        "Telemetry received | sequence=%s temperature=%s fan=%s mode=%s",
        telemetry["sequence"],
        telemetry["temperature_c"],
        "ON" if telemetry["fan_on"] else "OFF",
        "MANUAL" if telemetry["manual_mode"] else "AUTO",
    )

    return jsonify(
        {
            "ok": True,
            "message": "Telemetry received.",
            "sequence": event["sequence"],
            "saved_to_supabase": saved_to_supabase,
            "received_at": event["created_at"],
        }
    ), 201


@app.get("/api/comando")
def get_gateway_command():
    command = get_command_snapshot()

    return jsonify(
        {
            "command": command["command"],
            "command_id": command["command_id"],
        }
    )


@app.get("/api/estado")
def get_api_state():
    return jsonify(
        {
            "ok": True,
            "state": get_state_snapshot(),
            "command": get_command_snapshot(),
        }
    )


@app.get("/health")
def health():
    return jsonify(
        {
            "ok": True,
            "service": "supreme-fan-flask",
            "supabase_configured": supabase is not None,
            "device_online": get_state_snapshot()["online"],
            "time": utc_now_iso(),
        }
    )


@app.errorhandler(404)
def not_found(_error: Exception):
    if request.path.startswith("/api/"):
        return jsonify({"ok": False, "error": "Endpoint not found."}), 404
    return redirect(url_for("dashboard"))


@app.errorhandler(413)
def request_too_large(_error: Exception):
    return jsonify(
        {
            "ok": False,
            "error": "The request body is too large.",
        }
    ), 413


if __name__ == "__main__":
    debug_mode = os.getenv("FLASK_DEBUG", "0") == "1"
    app.run(
        host="0.0.0.0",
        port=int(os.getenv("PORT", "5000")),
        debug=debug_mode,
        threaded=True,
    )
