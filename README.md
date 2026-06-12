# DVT Guard AI Monitoring System

Graduation project prototype for real-time Deep Vein Thrombosis monitoring.

The ESP32 only reads sensors, calculates EMG RMS, sends values to Firebase, receives alarm commands, and executes the alarm. All medical decisions are made by the cloud Random Forest model plus the 10-second persistence timer.

## Architecture

```text
ESP32 sensors
  -> Firebase Realtime Database
  -> Flask service on Render
  -> RandomForest.pkl prediction
  -> Firebase prediction/alarm/logs
  -> Live web dashboard
  -> EmailJS alert
  -> ESP32 buzzer/red LED/OLED alarm
```

## Included Files

- `server/app.py` - Flask app, Firebase Admin integration, ML listener, admin APIs, EmailJS sender.
- `server/RandomForest.pkl` - your trained Random Forest model.
- `web/` - English live dashboard.
- `firmware/dvt_esp32_firebase/dvt_esp32_firebase.ino` - ESP32 firmware.
- `firebase/database.rules.json` - Realtime Database security rules.
- `requirements.txt`, `Procfile`, `render.yaml` - Render deployment files.

## Firebase Setup

1. Open Firebase Console.
2. Enable Authentication -> Sign-in method -> Email/Password.
3. Open Realtime Database and use:
   `https://dvt-monitoring-system-default-rtdb.firebaseio.com`
4. Upload the rules from:
   `firebase/database.rules.json`
5. Keep your service account JSON private. Do not put it in GitHub.

## Render Free Deployment

Create a new Render Web Service from this project folder.

Build command:

```bash
pip install -r requirements.txt
```

Start command:

```bash
gunicorn server.app:app --workers 1 --threads 4 --timeout 120
```

Use one worker only. More than one worker can create duplicate Firebase listeners and duplicate emails.

Required environment variables:

```text
FIREBASE_DATABASE_URL=https://dvt-monitoring-system-default-rtdb.firebaseio.com
MODEL_PATH=./server/RandomForest.pkl
DVT_PERSISTENCE_SECONDS=10
ADMIN_EMAIL=your-admin-email@example.com
ADMIN_INITIAL_PASSWORD=your-temporary-admin-password
ADMIN_NAME=DVT Admin
```

Service account option:

```text
FIREBASE_SERVICE_ACCOUNT_BASE64=base64-of-service-account-json
```

PowerShell command to create the base64 value:

```powershell
[Convert]::ToBase64String([IO.File]::ReadAllBytes("C:\Users\karee\Downloads\dvt-monitoring-system-firebase-adminsdk-fbsvc-a52fc09a6c.json"))
```

Render Free can sleep after inactivity. This is acceptable for the graduation prototype, but it is not production-grade 24/7 medical availability.

## EmailJS Setup

Create an EmailJS service and template. Add these Render environment variables:

```text
EMAILJS_SERVICE_ID=
EMAILJS_TEMPLATE_ID=
EMAILJS_PUBLIC_KEY=
EMAILJS_PRIVATE_KEY=
DEFAULT_ALERT_EMAIL=
```

Recommended EmailJS template variables:

```text
to_email
doctor_name
patient_id
patient_name
device_id
emg_signal
temperature
motion_value
confidence
dvt_timer
timestamp
subject
message
```

Suggested subject:

```text
DVT Risk Alert
```

Suggested body:

```text
Potential Deep Vein Thrombosis detected.

Patient: {{patient_name}} ({{patient_id}})
Device: {{device_id}}

Current Readings:
EMG Signal: {{emg_signal}}
Temperature: {{temperature}}
Motion Value: {{motion_value}}
Confidence: {{confidence}}%

The DVT condition persisted for more than {{dvt_timer}} seconds.
Immediate medical review is recommended.
```

If EmailJS variables are missing, the system still activates Firebase alarm and logs a notification, but no email is sent.

## Dashboard Workflow

1. Deploy to Render.
2. Open the Render URL.
3. Sign in with `ADMIN_EMAIL` and `ADMIN_INITIAL_PASSWORD`.
4. Add doctors.
5. Add patients with medical history.
6. Add ESP32 devices.
7. Link each doctor to the patients they should see.
8. Link each device to the correct patient.

Doctors can only see their linked patients.

## ESP32 Setup

Board:

```text
ESP32-WROOM-32E / ESP32 Dev Module
```

Arduino libraries:

```text
Firebase ESP Client by Mobizt
MPU6050_tockn
U8g2
OneWire
DallasTemperature
```

Current pin map:

```text
EMG_PIN       34
DETECT_PIN    35
DS18B20        27
BTN_BLUE       33
BTN_RED        26
BTN_GREEN      32
BUZZER          5
LED_RED        18
LED_GREEN      19
I2C SDA        21
I2C SCL        22
OLED           SH1106 128x64
```

Before uploading firmware:

1. In the dashboard, create device `device_001`.
2. Use the same device email and password in:
   `firmware/dvt_esp32_firebase/dvt_esp32_firebase.ino`
3. Confirm:

```cpp
#define DEVICE_ID "device_001"
#define PATIENT_ID "patient_001"
#define DEVICE_EMAIL "device_001@dvt.local"
#define DEVICE_PASSWORD "ChangeMe#2026"
```

The red button is the only physical action that acknowledges and stops the active alarm on the device. If the cloud model still detects continuous DVT risk after acknowledgement, the server can trigger the alarm again after a new 10-second persistence window.

## Firebase Data Shape

```text
devices/{deviceId}/
  patientId
  sensors/
    EMG_Signal
    Temperature
    Motion_Value
    Timestamp
  prediction/
    Status
    Label
    Confidence
    DVT_Probability
    DVT_Timer
    LastUpdated
  alarm/
    Active
    TriggeredAt
    AcknowledgedAt

patients/{patientId}/
  name
  age
  gender
  medicalHistory
  notes

doctors/{uid}/
  name
  email
  role

doctorPatients/{doctorUid}/{patientId}
patientDoctors/{patientId}/{doctorUid}
patientDevices/{patientId}/{deviceId}
logs/{patientId}/{logId}
notifications/{patientId}/{notificationId}
```

## Medical Safety Note

This is a graduation project prototype. It is not a certified medical device and must not be used as a real clinical decision system without formal validation, safety testing, regulatory review, and production-grade hosting.
