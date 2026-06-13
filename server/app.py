import base64
import json
import logging
import os
import secrets
import string
import threading
import urllib.parse
import time
from datetime import datetime, timezone
from pathlib import Path
from urllib import error, request as urlrequest
import smtplib
from email.mime.text import MIMEText
from email.mime.multipart import MIMEMultipart

import firebase_admin
import joblib
import numpy as np
import pandas as pd
from firebase_admin import auth, credentials, db
from flask import Flask, jsonify, render_template, request


BASE_DIR = Path(__file__).resolve().parent.parent
MODEL_PATH = Path(os.environ.get("MODEL_PATH", BASE_DIR / "server" / "RandomForest.pkl"))
DATABASE_URL = os.environ.get(
    "FIREBASE_DATABASE_URL",
    "https://dvt-monitoring-system-default-rtdb.firebaseio.com",
)

FEATURE_COLUMNS = ["EMG_Signal", "Temperature", "Motion_Value"]
STATUS_HEALTHY = "Healthy"
STATUS_RISK = "DVT Risk"
PERSISTENCE_SECONDS = int(os.environ.get("DVT_PERSISTENCE_SECONDS", "10"))

FIREBASE_WEB_CONFIG = {
    "apiKey": "AIzaSyCmW3A9lypdBna_lNz6hmlhAVBPkTCpN1A",
    "authDomain": "dvt-monitoring-system.firebaseapp.com",
    "databaseURL": "https://dvt-monitoring-system-default-rtdb.firebaseio.com",
    "projectId": "dvt-monitoring-system",
    "storageBucket": "dvt-monitoring-system.firebasestorage.app",
    "messagingSenderId": "79038955234",
    "appId": "1:79038955234:web:bed042097c891033c6e8bb",
}

logging.basicConfig(
    level=os.environ.get("LOG_LEVEL", "INFO"),
    format="%(asctime)s %(levelname)s %(message)s",
)
logger = logging.getLogger("dvt-monitoring")

app = Flask(
    __name__,
    template_folder=str(BASE_DIR / "web" / "templates"),
    static_folder=str(BASE_DIR / "web" / "static"),
    static_url_path="/static",
)

_model = None
_listener_started = False
_listener_lock = threading.Lock()
_fingerprints = {}


def utc_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def epoch_ms() -> int:
    return int(time.time() * 1000)


def sanitize_key(value: str) -> str:
    cleaned = "".join("_" if ch in ".#$[]/" else ch for ch in str(value).strip())
    return cleaned or f"id_{epoch_ms()}"


def generated_password(length: int = 18) -> str:
    alphabet = string.ascii_letters + string.digits + "!@#%?"
    while True:
        password = "".join(secrets.choice(alphabet) for _ in range(length))
        if (
            any(c.islower() for c in password)
            and any(c.isupper() for c in password)
            and any(c.isdigit() for c in password)
        ):
            return password


def load_service_account_credential():
    raw_json = os.environ.get("FIREBASE_SERVICE_ACCOUNT_JSON")
    raw_b64 = os.environ.get("FIREBASE_SERVICE_ACCOUNT_BASE64")
    path = os.environ.get("FIREBASE_SERVICE_ACCOUNT_PATH")

    if raw_json:
        return credentials.Certificate(json.loads(raw_json))

    if raw_b64:
        decoded = base64.b64decode(raw_b64.encode("utf-8")).decode("utf-8")
        return credentials.Certificate(json.loads(decoded))

    if path:
        return credentials.Certificate(path)

    raise RuntimeError(
        "Firebase service account is missing. Set FIREBASE_SERVICE_ACCOUNT_JSON, "
        "FIREBASE_SERVICE_ACCOUNT_BASE64, or FIREBASE_SERVICE_ACCOUNT_PATH."
    )


def init_firebase():
    if firebase_admin._apps:
        return

    cred = load_service_account_credential()
    firebase_admin.initialize_app(cred, {"databaseURL": DATABASE_URL})
    logger.info("Firebase Admin initialized for %s", DATABASE_URL)


def load_model():
    global _model
    if _model is not None:
        return _model

    if not MODEL_PATH.exists():
        raise FileNotFoundError(f"Model file was not found at {MODEL_PATH}")

    _model = joblib.load(MODEL_PATH)
    logger.info("Random Forest model loaded from %s", MODEL_PATH)
    return _model

def send_gmail(to_email, subject, body):
    try:
        msg = MIMEMultipart()

        msg["From"] = os.environ["GMAIL_EMAIL"]
        msg["To"] = to_email
        msg["Subject"] = subject

        msg.attach(MIMEText(body, "plain"))

        server = smtplib.SMTP("smtp.gmail.com", 587)
        server.starttls()

        server.login(
            os.environ["GMAIL_EMAIL"],
            os.environ["GMAIL_APP_PASSWORD"]
        )

        server.send_message(msg)
        server.quit()

        logger.info("GMAIL EMAIL SENT SUCCESSFULLY")

        return {
            "sent": True
        }

    except Exception as e:
        logger.exception("GMAIL SEND FAILED")

        return {
            "sent": False,
            "reason": str(e)
        }

def send_telegram(message):
    try:
        token = os.environ["TELEGRAM_BOT_TOKEN"]
        chat_id = os.environ["TELEGRAM_CHAT_ID"]

        url = (
            f"https://api.telegram.org/bot{token}/sendMessage"
            f"?chat_id={chat_id}"
            f"&text={urllib.parse.quote(message)}"
        )

        with urlrequest.urlopen(url, timeout=10) as response:
            response.read()

        logger.info("TELEGRAM MESSAGE SENT")

        return {"sent": True}

    except Exception as e:
        logger.exception("TELEGRAM SEND FAILED")

        return {
            "sent": False,
            "reason": str(e)
        }
    
def emailjs_configured() -> bool:
    return all(
        os.environ.get(name)
        for name in ("EMAILJS_SERVICE_ID", "EMAILJS_TEMPLATE_ID", "EMAILJS_PUBLIC_KEY")
    )


def send_emailjs(template_params: dict) -> dict:
    if not emailjs_configured():
        
        logger.info("EmailJS NOT configured")
        return {"sent": False, "reason": "EmailJS is not configured"}

    logger.info("EMAILJS_SERVICE_ID=%s", os.environ.get("EMAILJS_SERVICE_ID"))
    logger.info("EMAILJS_TEMPLATE_ID=%s", os.environ.get("EMAILJS_TEMPLATE_ID"))
    logger.info("EMAILJS_PUBLIC_KEY=%s", os.environ.get("EMAILJS_PUBLIC_KEY"))
    logger.info("EMAILJS_PRIVATE_KEY_EXISTS=%s", bool(os.environ.get("EMAILJS_PRIVATE_KEY")))

    payload = {
        "service_id": os.environ["EMAILJS_SERVICE_ID"],
        "template_id": os.environ["EMAILJS_TEMPLATE_ID"],
        "user_id": os.environ["EMAILJS_PUBLIC_KEY"],
        "template_params": template_params,
    }

    # private key disabled for testing

    logger.info("Sending EmailJS request")

    data = json.dumps(payload).encode("utf-8")
    req = urlrequest.Request(
        "https://api.emailjs.com/api/v1.0/email/send",
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    try:
        with urlrequest.urlopen(req, timeout=12) as response:
            body = response.read().decode("utf-8", errors="replace")
            logger.info("EmailJS SUCCESS status=%s body=%s", response.status, body)
            return {"sent": 200 <= response.status < 300, "status": response.status, "body": body}

    except error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        logger.error("EmailJS HTTP ERROR status=%s body=%s", exc.code, body)
        return {
            "sent": False,
            "status": exc.code,
            "body": body,
        }

    except Exception as exc:
        logger.exception("EmailJS request failed")
        return {"sent": False, "reason": str(exc)}


def verify_bearer_token():
    auth_header = request.headers.get("Authorization", "")
    if not auth_header.startswith("Bearer "):
        return None

    token = auth_header.split(" ", 1)[1].strip()
    if not token:
        return None

    try:
        return auth.verify_id_token(token)
    except Exception:
        logger.exception("Invalid Firebase ID token")
        return None


def get_role(uid: str) -> str:
    if db.reference(f"admins/{uid}").get() is True:
        return "admin"
    doctor = db.reference(f"doctors/{uid}").get() or {}
    return doctor.get("role", "doctor")


def require_user():
    decoded = verify_bearer_token()
    if not decoded:
        return None, (jsonify({"error": "Unauthorized"}), 401)
    return decoded, None


def require_admin():
    decoded, error_response = require_user()
    if error_response:
        return None, error_response

    role = get_role(decoded["uid"])
    if role != "admin":
        return None, (jsonify({"error": "Admin role is required"}), 403)

    return decoded, None


def bootstrap_admin_user():
    email = os.environ.get("ADMIN_EMAIL")
    password = os.environ.get("ADMIN_INITIAL_PASSWORD")
    name = os.environ.get("ADMIN_NAME", "System Administrator")

    if not email or not password:
        logger.info("Admin bootstrap skipped. ADMIN_EMAIL or ADMIN_INITIAL_PASSWORD is missing.")
        return

    try:
        user = auth.get_user_by_email(email)
        logger.info("Admin auth user already exists: %s", email)
    except auth.UserNotFoundError:
        user = auth.create_user(email=email, password=password, display_name=name)
        logger.info("Created bootstrap admin auth user: %s", email)

    auth.set_custom_user_claims(user.uid, {"role": "admin"})
    updates = {
        f"admins/{user.uid}": True,
        f"doctors/{user.uid}": {
            "uid": user.uid,
            "name": name,
            "email": email,
            "role": "admin",
            "createdAt": utc_iso(),
            "updatedAt": utc_iso(),
        },
    }
    db.reference("/").update(updates)


def normalize_sensors(sensors: dict) -> dict:
    if not isinstance(sensors, dict):
        raise ValueError("Sensor payload is missing")

    values = {
        "EMG_Signal": float(sensors.get("EMG_Signal")),
        "Temperature": float(sensors.get("Temperature")),
        "Motion_Value": float(sensors.get("Motion_Value")),
        "Timestamp": sensors.get("Timestamp") or utc_iso(),
    }

    if values["Temperature"] <= -100:
        raise ValueError("Temperature sensor is not ready")

    return values


def model_predict(values: dict) -> dict:
    model = load_model()
    features = pd.DataFrame(
        [[values["EMG_Signal"], values["Temperature"], values["Motion_Value"]]],
        columns=FEATURE_COLUMNS,
    )
    raw_label = model.predict(features)[0]
    label = int(raw_label)
    status = STATUS_RISK if label == 1 else STATUS_HEALTHY

    confidence = None
    dvt_probability = None

    if hasattr(model, "predict_proba"):
        probabilities = model.predict_proba(features)[0]
        classes = [int(item) for item in getattr(model, "classes_", [])]
        if classes:
            predicted_index = classes.index(label) if label in classes else int(np.argmax(probabilities))
            confidence = float(probabilities[predicted_index]) * 100.0
            if 1 in classes:
                dvt_probability = float(probabilities[classes.index(1)]) * 100.0
        else:
            confidence = float(np.max(probabilities)) * 100.0

    return {
        "Label": label,
        "Status": status,
        "Confidence": round(confidence, 2) if confidence is not None else None,
        "DVT_Probability": round(dvt_probability, 2) if dvt_probability is not None else None,
    }


def ack_after_start(alarm: dict, risk_start_ms: int | None) -> bool:
    if not risk_start_ms:
        return False
    ack_ms = alarm.get("AcknowledgedAtMs")
    try:
        return int(ack_ms) >= int(risk_start_ms)
    except (TypeError, ValueError):
        return False


def resolve_patient_doctors(patient_id: str | None) -> list[dict]:
    if not patient_id:
        fallback = os.environ.get("DEFAULT_ALERT_EMAIL")
        return [{"email": fallback, "name": "Doctor", "uid": None}] if fallback else []

    doctor_links = db.reference(f"patientDoctors/{patient_id}").get() or {}
    doctors = []
    for uid, enabled in doctor_links.items():
        if not enabled:
            continue
        doctor = db.reference(f"doctors/{uid}").get() or {}
        if doctor.get("email"):
            doctors.append(doctor)

    if not doctors:
        patient = db.reference(f"patients/{patient_id}").get() or {}
        uid = patient.get("primaryDoctorUid")
        if uid:
            doctor = db.reference(f"doctors/{uid}").get() or {}
            if doctor.get("email"):
                doctors.append(doctor)

    fallback = os.environ.get("DEFAULT_ALERT_EMAIL")
    if not doctors and fallback:
        doctors.append({"email": fallback, "name": "Doctor", "uid": None})

    return doctors


def write_notification(patient_id: str, notification_id: str, payload: dict):
    db.reference(f"notifications/{patient_id}/{notification_id}").set(payload)


def send_alarm_notifications(patient_id: str | None, device_id: str, sensors: dict, prediction: dict, timer: int):
    patient_key = patient_id or "_unassigned"
    patient = db.reference(f"patients/{patient_key}").get() or {}
    doctors = resolve_patient_doctors(patient_id)
    notification_id = f"{epoch_ms()}_{device_id}"
    results = []

    for doctor in doctors:
        params = {
            "to_email": doctor.get("email", ""),
            "doctor_name": doctor.get("name", "Doctor"),
            "patient_id": patient_id or "Unassigned",
            "patient_name": patient.get("name", "Unassigned Patient"),
            "device_id": device_id,
            "emg_signal": sensors["EMG_Signal"],
            "temperature": sensors["Temperature"],
            "motion_value": sensors["Motion_Value"],
            "confidence": prediction.get("Confidence"),
            "dvt_timer": timer,
            "timestamp": sensors.get("Timestamp") or utc_iso(),
            "subject": "DVT Risk Alert",
            "message": (
                "Potential Deep Vein Thrombosis detected. The DVT condition persisted "
                "for more than 10 seconds. Immediate medical review is recommended."
            ),
        }
        body = f"""
Potential Deep Vein Thrombosis detected.

Patient: {patient.get('name', 'Unknown')}
Patient ID: {patient_id}

Device ID: {device_id}

EMG Signal: {sensors['EMG_Signal']}
Temperature: {sensors['Temperature']}
Motion Value: {sensors['Motion_Value']}

Confidence: {prediction.get('Confidence')}%

The DVT condition persisted for more than {timer} seconds.

Immediate medical review is recommended.
"""

        result = send_telegram(body)
        results.append({"doctorEmail": doctor.get("email"), **result})

    write_notification(
        patient_key,
        notification_id,
        {
            "type": "DVT_RISK_ALERT",
            "patientId": patient_id,
            "deviceId": device_id,
            "createdAt": utc_iso(),
            "read": False,
            "emailResults": results,
            "sensors": sensors,
            "prediction": prediction,
        },
    )
    return results


def process_device_reading(device_id: str, device_snapshot: dict | None = None):
    if device_snapshot is None:
        device_snapshot = db.reference(f"devices/{device_id}").get() or {}

    sensors = normalize_sensors(device_snapshot.get("sensors") or {})
    fingerprint = json.dumps(
        [sensors["EMG_Signal"], sensors["Temperature"], sensors["Motion_Value"], sensors["Timestamp"]],
        sort_keys=True,
    )
    if _fingerprints.get(device_id) == fingerprint:
        return
    _fingerprints[device_id] = fingerprint

    prediction = model_predict(sensors)
    now = epoch_ms()
    previous_prediction = device_snapshot.get("prediction") or {}
    alarm = device_snapshot.get("alarm") or {}
    patient_id = device_snapshot.get("patientId") or (device_snapshot.get("meta") or {}).get("patientId")

    previous_start = previous_prediction.get("DVTStartedAtMs")
    risk_start_ms = None
    timer_seconds = 0

    if prediction["Label"] == 1:
        if previous_start and not ack_after_start(alarm, int(previous_start)):
            risk_start_ms = int(previous_start)
        else:
            risk_start_ms = now
        timer_seconds = max(0, int((now - risk_start_ms) / 1000))

    alarm_active = bool(alarm.get("Active", False))
    trigger_now = prediction["Label"] == 1 and timer_seconds >= PERSISTENCE_SECONDS and not alarm_active
    if trigger_now:
        alarm_active = True

    prediction_payload = {
        **prediction,
        "DVT_Timer": timer_seconds,
        "DVTStartedAtMs": risk_start_ms,
        "LastUpdated": utc_iso(),
        "LastUpdatedMs": now,
        "Model": "RandomForest.pkl",
        "FeatureOrder": FEATURE_COLUMNS,
    }

    log_key = f"{now}_{sanitize_key(device_id)}"
    log_patient = sanitize_key(patient_id or "_unassigned")
    alarm_payload = {
        "Active": alarm_active,
        "UpdatedAt": utc_iso(),
        "UpdatedAtMs": now,
    }
    if trigger_now:
        alarm_payload.update(
            {
                "TriggeredAt": utc_iso(),
                "TriggeredAtMs": now,
                "TriggerId": log_key,
                "AcknowledgedAt": None,
                "AcknowledgedAtMs": None,
            }
        )

    history_payload = {
        "patientId": patient_id,
        "deviceId": device_id,
        "sensors": sensors,
        "prediction": prediction_payload,
        "alarm": {"Active": alarm_active},
        "createdAt": utc_iso(),
        "createdAtMs": now,
    }

    updates = {
        f"devices/{device_id}/prediction": prediction_payload,
        f"devices/{device_id}/alarm": {**alarm, **alarm_payload},
        f"logs/{log_patient}/{log_key}": history_payload,
        f"deviceLogs/{device_id}/{log_key}": history_payload,
    }
    db.reference("/").update(updates)

    if trigger_now:
        results = send_alarm_notifications(patient_id, device_id, sensors, prediction_payload, timer_seconds)
        db.reference(f"devices/{device_id}/alarm/EmailResults").set(results)

    logger.info(
        "Processed %s: %s, confidence=%s, timer=%ss, alarm=%s",
        device_id,
        prediction["Status"],
        prediction.get("Confidence"),
        timer_seconds,
        alarm_active,
    )


def handle_devices_event(event):
    path = event.path or "/"
    pieces = [part for part in path.split("/") if part]

    try:
        if path == "/" and isinstance(event.data, dict):
            for device_id, device_data in event.data.items():
                if isinstance(device_data, dict) and device_data.get("sensors"):
                    process_device_reading(device_id, device_data)
            return

        if len(pieces) >= 2 and pieces[1] == "sensors":
            process_device_reading(pieces[0])
    except Exception:
        logger.exception("Failed to process Firebase event at %s", path)


def firebase_listener_loop():
    while True:
        try:
            logger.info("Starting Firebase Realtime Database listener")
            listener = db.reference("devices").listen(handle_devices_event)
            while True:
                time.sleep(3600)
        except Exception:
            logger.exception("Firebase listener stopped; retrying in 5 seconds")
            time.sleep(5)
        finally:
            try:
                listener.close()
            except Exception:
                pass


def start_listener_once():
    global _listener_started
    if os.environ.get("DISABLE_FIREBASE_LISTENER") == "1":
        logger.info("Firebase listener disabled by DISABLE_FIREBASE_LISTENER")
        return

    with _listener_lock:
        if _listener_started:
            return
        thread = threading.Thread(target=firebase_listener_loop, daemon=True)
        thread.start()
        _listener_started = True


@app.route("/")
def index():
    return render_template("index.html")


@app.get("/health")
def health():
    return jsonify({"ok": True, "time": utc_iso(), "listenerStarted": _listener_started})


@app.get("/api/config")
def api_config():
    return jsonify({"firebase": FIREBASE_WEB_CONFIG})


@app.get("/api/session")
def api_session():
    decoded, error_response = require_user()
    if error_response:
        return error_response

    uid = decoded["uid"]
    doctor = db.reference(f"doctors/{uid}").get() or {}
    return jsonify(
        {
            "uid": uid,
            "email": decoded.get("email"),
            "name": doctor.get("name") or decoded.get("name") or decoded.get("email"),
            "role": get_role(uid),
        }
    )


@app.post("/api/admin/doctors")
def api_create_doctor():
    decoded, error_response = require_admin()
    if error_response:
        return error_response

    data = request.get_json(silent=True) or {}
    email = (data.get("email") or "").strip().lower()
    name = (data.get("name") or "").strip() or email
    password = data.get("password")
    role = data.get("role") if data.get("role") in ("admin", "doctor") else "doctor"

    if not email:
        return jsonify({"error": "Doctor email is required"}), 400

    try:
        user = auth.get_user_by_email(email)
        if password:
            auth.update_user(user.uid, password=password, display_name=name)
        else:
            auth.update_user(user.uid, display_name=name)
    except auth.UserNotFoundError:
        if not password:
            return jsonify({"error": "Password is required for a new doctor"}), 400
        user = auth.create_user(email=email, password=password, display_name=name)

    auth.set_custom_user_claims(user.uid, {"role": role})
    updates = {
        f"doctors/{user.uid}": {
            "uid": user.uid,
            "name": name,
            "email": email,
            "role": role,
            "updatedAt": utc_iso(),
        }
    }
    if role == "admin":
        updates[f"admins/{user.uid}"] = True

    db.reference("/").update(updates)
    return jsonify({"ok": True, "uid": user.uid, "email": email, "role": role})


@app.post("/api/admin/patients")
def api_upsert_patient():
    decoded, error_response = require_admin()
    if error_response:
        return error_response

    data = request.get_json(silent=True) or {}
    patient_id = sanitize_key(data.get("patientId") or f"patient_{epoch_ms()}")
    payload = {
        "patientId": patient_id,
        "name": data.get("name") or patient_id,
        "age": data.get("age") or "",
        "gender": data.get("gender") or "",
        "medicalHistory": data.get("medicalHistory") or "",
        "notes": data.get("notes") or "",
        "updatedAt": utc_iso(),
    }
    existing = db.reference(f"patients/{patient_id}").get() or {}
    if not existing.get("createdAt"):
        payload["createdAt"] = utc_iso()
    db.reference(f"patients/{patient_id}").update(payload)
    return jsonify({"ok": True, "patientId": patient_id})


@app.post("/api/admin/devices")
def api_create_device():
    decoded, error_response = require_admin()
    if error_response:
        return error_response

    data = request.get_json(silent=True) or {}
    device_id = sanitize_key(data.get("deviceId") or f"device_{epoch_ms()}")
    device_email = (data.get("deviceEmail") or f"{device_id}@dvt.local").strip().lower()
    password = data.get("devicePassword") or generated_password()
    generated = not bool(data.get("devicePassword"))
    label = data.get("label") or device_id
    patient_id = sanitize_key(data["patientId"]) if data.get("patientId") else None

    try:
        user = auth.get_user_by_email(device_email)
        auth.update_user(user.uid, password=password, display_name=device_id)
    except auth.UserNotFoundError:
        user = auth.create_user(email=device_email, password=password, display_name=device_id)

    auth.set_custom_user_claims(user.uid, {"device": True, "deviceId": device_id})

    updates = {
        f"deviceAuth/{user.uid}": {"deviceId": device_id, "email": device_email},
        f"devices/{device_id}/deviceId": device_id,
        f"devices/{device_id}/label": label,
        f"devices/{device_id}/deviceEmail": device_email,
        f"devices/{device_id}/deviceAuthUid": user.uid,
        f"devices/{device_id}/updatedAt": utc_iso(),
        f"devices/{device_id}/alarm/Active": False,
    }
    if patient_id:
        updates.update(link_device_patient_updates(device_id, patient_id))

    db.reference("/").update(updates)
    response = {"ok": True, "deviceId": device_id, "deviceEmail": device_email, "deviceUid": user.uid}
    if generated:
        response["generatedPassword"] = password
    return jsonify(response)


def link_device_patient_updates(device_id: str, patient_id: str) -> dict:
    previous = db.reference(f"devices/{device_id}/patientId").get()
    updates = {
        f"devices/{device_id}/patientId": patient_id,
        f"patients/{patient_id}/deviceId": device_id,
        f"patientDevices/{patient_id}/{device_id}": True,
    }
    if previous and previous != patient_id:
        updates[f"patientDevices/{previous}/{device_id}"] = None
    return updates


@app.post("/api/admin/link-device-patient")
def api_link_device_patient():
    decoded, error_response = require_admin()
    if error_response:
        return error_response

    data = request.get_json(silent=True) or {}
    device_id = sanitize_key(data.get("deviceId") or "")
    patient_id = sanitize_key(data.get("patientId") or "")
    if not device_id or not patient_id:
        return jsonify({"error": "deviceId and patientId are required"}), 400

    db.reference("/").update(link_device_patient_updates(device_id, patient_id))
    return jsonify({"ok": True, "deviceId": device_id, "patientId": patient_id})


@app.post("/api/admin/link-doctor-patient")
def api_link_doctor_patient():
    decoded, error_response = require_admin()
    if error_response:
        return error_response

    data = request.get_json(silent=True) or {}
    doctor_uid = sanitize_key(data.get("doctorUid") or "")
    patient_id = sanitize_key(data.get("patientId") or "")
    if not doctor_uid or not patient_id:
        return jsonify({"error": "doctorUid and patientId are required"}), 400

    updates = {
        f"doctorPatients/{doctor_uid}/{patient_id}": True,
        f"patientDoctors/{patient_id}/{doctor_uid}": True,
        f"patients/{patient_id}/primaryDoctorUid": doctor_uid,
    }
    db.reference("/").update(updates)
    return jsonify({"ok": True, "doctorUid": doctor_uid, "patientId": patient_id})


def initialize_application():
    init_firebase()
    load_model()
    bootstrap_admin_user()
    start_listener_once()


try:
    initialize_application()
except Exception:
    logger.exception("Application initialization failed")


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=int(os.environ.get("PORT", "5000")), debug=True)
