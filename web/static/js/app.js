import { initializeApp } from "https://www.gstatic.com/firebasejs/12.14.0/firebase-app.js";
import {
  getAuth,
  onAuthStateChanged,
  signInWithEmailAndPassword,
  signOut,
} from "https://www.gstatic.com/firebasejs/12.14.0/firebase-auth.js";
import {
  getDatabase,
  limitToLast,
  onValue,
  query,
  ref,
} from "https://www.gstatic.com/firebasejs/12.14.0/firebase-database.js";

const $ = (id) => document.getElementById(id);

const state = {
  auth: null,
  db: null,
  session: null,
  patients: {},
  devices: {},
  doctors: {},
  patientDevices: {},
  selectedPatientId: null,
  selectedDeviceId: null,
  unsubs: [],
  scopedUnsubs: [],
  deviceUnsubs: {},
  historyUnsub: null,
};

function fmtNumber(value, decimals = 1) {
  const n = Number(value);
  return Number.isFinite(n) ? n.toFixed(decimals) : "-";
}

function fmtDate(value) {
  if (!value) return "-";
  const date = typeof value === "number" ? new Date(value) : new Date(value);
  if (Number.isNaN(date.getTime())) return String(value);
  return date.toLocaleString();
}

function setConnection(text, mode = "neutral") {
  const badge = $("connectionBadge");
  badge.textContent = text;
  badge.className = `status-pill ${mode}`;
}

function showLogin() {
  $("loginView").classList.remove("hidden");
  $("appView").classList.add("hidden");
}

function showApp() {
  $("loginView").classList.add("hidden");
  $("appView").classList.remove("hidden");
}

async function api(path, body = undefined) {
  const user = state.auth.currentUser;
  if (!user) throw new Error("Not signed in");
  const token = await user.getIdToken();
  const response = await fetch(path, {
    method: body ? "POST" : "GET",
    headers: {
      Authorization: `Bearer ${token}`,
      "Content-Type": "application/json",
    },
    body: body ? JSON.stringify(body) : undefined,
  });
  const data = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(data.error || `Request failed: ${response.status}`);
  return data;
}

function clearUnsubs(bucket = "unsubs") {
  state[bucket].forEach((unsubscribe) => unsubscribe());
  state[bucket] = [];
}

function clearDeviceUnsubs() {
  Object.values(state.deviceUnsubs).forEach((unsubscribe) => unsubscribe());
  state.deviceUnsubs = {};
}

function subscribe(path, handler, bucket = "unsubs") {
  const unsubscribe = onValue(ref(state.db, path), (snapshot) => handler(snapshot.val()));
  state[bucket].push(unsubscribe);
  return unsubscribe;
}

function subscribeDevice(deviceId) {
  if (!deviceId || state.deviceUnsubs[deviceId]) return;
  state.deviceUnsubs[deviceId] = onValue(ref(state.db, `devices/${deviceId}`), (snapshot) => {
    const value = snapshot.val();
    if (value) {
      state.devices[deviceId] = value;
    } else {
      delete state.devices[deviceId];
    }
    renderPatients();
    renderDashboard();
  });
}

function patientDeviceIds(patientId) {
  const linked = Object.keys(state.patientDevices[patientId] || {}).filter(
    (deviceId) => state.patientDevices[patientId][deviceId],
  );
  const fallback = Object.entries(state.devices)
    .filter(([, device]) => device?.patientId === patientId)
    .map(([deviceId]) => deviceId);
  return [...new Set([...linked, ...fallback])].sort();
}

function currentPatientDevice(patientId) {
  const ids = patientDeviceIds(patientId);
  if (!ids.length) return [null, null];
  if (!state.selectedDeviceId || !ids.includes(state.selectedDeviceId)) {
    state.selectedDeviceId = ids[0];
  }
  return [state.selectedDeviceId, state.devices[state.selectedDeviceId] || null];
}

function patientMode(patientId) {
  const ids = patientDeviceIds(patientId);
  for (const deviceId of ids) {
    const device = state.devices[deviceId];
    if (device?.alarm?.Active) return "alarm";
    if (device?.prediction?.Status === "DVT Risk") return "risk";
  }
  return "healthy";
}

function renderPatients() {
  const container = $("patientList");
  container.replaceChildren();
  const entries = Object.entries(state.patients).sort((a, b) => {
    const an = a[1]?.name || a[0];
    const bn = b[1]?.name || b[0];
    return an.localeCompare(bn);
  });

  if (!entries.length) {
    const empty = document.createElement("p");
    empty.className = "form-message";
    empty.textContent = state.session?.role === "admin" ? "No patients yet." : "No assigned patients yet.";
    container.appendChild(empty);
    return;
  }

  for (const [patientId, patient] of entries) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `patient-item ${patientId === state.selectedPatientId ? "active" : ""}`;
    button.addEventListener("click", () => selectPatient(patientId));

    const label = document.createElement("div");
    label.textContent = patient?.name || patientId;
    const sub = document.createElement("span");
    sub.textContent = patientId;
    label.appendChild(sub);

    const dot = document.createElement("i");
    dot.className = `patient-dot ${patientMode(patientId)}`;

    button.append(label, dot);
    container.appendChild(button);
  }
}

function setStatusPanel(mode, title, subtext) {
  const panel = $("clinicalStatusPanel");
  panel.className = `clinical-status ${mode}`;
  $("predictionStatus").textContent = title;
  $("predictionSubtext").textContent = subtext;
  setConnection(title, mode);
}

function renderDashboard() {
  const patientId = state.selectedPatientId;
  if (!patientId) {
    $("emptyState").classList.remove("hidden");
    $("dashboardView").classList.add("hidden");
    return;
  }

  const patient = state.patients[patientId];
  const [deviceId, device] = currentPatientDevice(patientId);
  $("emptyState").classList.add("hidden");
  $("dashboardView").classList.remove("hidden");

  $("patientTitle").textContent = patient?.name || patientId;
  $("patientIdText").textContent = patientId;
  $("deviceIdText").textContent = deviceId || "No linked device";
  $("patientMetaText").textContent = [patient?.age ? `${patient.age} years` : "", patient?.gender || ""]
    .filter(Boolean)
    .join(" | ") || "Patient profile";
  $("medicalHistoryText").textContent = patient?.medicalHistory || "No medical history has been added yet.";

  const sensors = device?.sensors || {};
  const prediction = device?.prediction || {};
  const alarm = device?.alarm || {};
  const alarmActive = alarm.Active === true;

  $("emgValue").textContent = fmtNumber(sensors.EMG_Signal, 0);
  $("tempValue").textContent = sensors.Temperature !== undefined ? `${fmtNumber(sensors.Temperature, 1)} C` : "-";
  $("motionValue").textContent = fmtNumber(sensors.Motion_Value, 2);
  $("confidenceValue").textContent = prediction.Confidence !== undefined ? `${fmtNumber(prediction.Confidence, 1)}%` : "-";
  $("timerValue").textContent = `${Number(prediction.DVT_Timer || 0)}s`;
  $("lastUpdateText").textContent = fmtDate(prediction.LastUpdated || sensors.Timestamp);
  $("alarmText").textContent = alarmActive ? "Activated" : "Inactive";

  if (!deviceId) {
    setStatusPanel("neutral", "No Device", "Link an ESP32 device to this patient to begin monitoring.");
  } else if (alarmActive) {
    setStatusPanel("alarm", "Alarm Activated", "The alarm is latched and will stop only after the red button is pressed on the ESP32 device.");
  } else if (prediction.Status === "DVT Risk") {
    setStatusPanel("risk", "DVT Risk", "The model is detecting risk. Alarm triggers only after a continuous 10-second persistence window.");
  } else if (prediction.Status === "Healthy") {
    setStatusPanel("healthy", "Healthy", "Current readings are classified as healthy by the Random Forest model.");
  } else {
    setStatusPanel("neutral", "Waiting", "Sensor readings have not been processed yet.");
  }
}

function renderHistory(logs = {}) {
  const body = $("historyBody");
  body.replaceChildren();
  const entries = Object.entries(logs)
    .sort((a, b) => (b[1]?.createdAtMs || 0) - (a[1]?.createdAtMs || 0))
    .slice(0, 50);

  if (!entries.length) {
    const row = document.createElement("tr");
    const cell = document.createElement("td");
    cell.colSpan = 6;
    cell.textContent = "No historical readings yet.";
    row.appendChild(cell);
    body.appendChild(row);
    return;
  }

  for (const [, log] of entries) {
    const row = document.createElement("tr");
    const sensors = log.sensors || {};
    const prediction = log.prediction || {};
    const alarm = log.alarm || {};
    const cells = [
      fmtDate(log.createdAt || sensors.Timestamp),
      fmtNumber(sensors.EMG_Signal, 0),
      sensors.Temperature !== undefined ? `${fmtNumber(sensors.Temperature, 1)} C` : "-",
      fmtNumber(sensors.Motion_Value, 2),
      prediction.Status || "-",
      alarm.Active ? "Active" : "Inactive",
    ];

    for (const value of cells) {
      const cell = document.createElement("td");
      cell.textContent = value;
      row.appendChild(cell);
    }
    body.appendChild(row);
  }
}

function subscribeHistory(patientId) {
  if (state.historyUnsub) {
    state.historyUnsub();
    state.historyUnsub = null;
  }
  state.historyUnsub = onValue(
    query(ref(state.db, `logs/${patientId}`), limitToLast(50)),
    (snapshot) => renderHistory(snapshot.val() || {}),
  );
}

function selectPatient(patientId) {
  state.selectedPatientId = patientId;
  state.selectedDeviceId = null;
  subscribeHistory(patientId);
  renderPatients();
  renderDashboard();
}

function setOptions(select, entries, valueSelector, labelSelector, emptyLabel = "Select") {
  select.replaceChildren();
  const empty = document.createElement("option");
  empty.value = "";
  empty.textContent = emptyLabel;
  select.appendChild(empty);

  for (const item of entries) {
    const option = document.createElement("option");
    option.value = valueSelector(item);
    option.textContent = labelSelector(item);
    select.appendChild(option);
  }
}

function renderAdminOptions() {
  if (state.session?.role !== "admin") return;

  const patients = Object.entries(state.patients).sort((a, b) => a[0].localeCompare(b[0]));
  const doctors = Object.entries(state.doctors)
    .filter(([, doctor]) => doctor?.email)
    .sort((a, b) => (a[1].name || a[1].email).localeCompare(b[1].name || b[1].email));
  const devices = Object.entries(state.devices).sort((a, b) => a[0].localeCompare(b[0]));

  const patientLabel = ([id, patient]) => `${patient?.name || id} (${id})`;
  setOptions($("devicePatientSelect"), patients, ([id]) => id, patientLabel, "No patient yet");
  setOptions($("doctorPatientSelect"), patients, ([id]) => id, patientLabel, "Select patient");
  setOptions($("patientDeviceSelect"), patients, ([id]) => id, patientLabel, "Select patient");
  setOptions($("doctorSelect"), doctors, ([uid]) => uid, ([, doctor]) => `${doctor.name || doctor.email} (${doctor.email})`, "Select doctor");
  setOptions($("deviceSelect"), devices, ([id]) => id, ([id, device]) => `${device.label || id} (${id})`, "Select device");
}

function renderAll() {
  renderPatients();
  renderDashboard();
  renderAdminOptions();
}

function subscribeAdminData() {
  subscribe("patients", (value) => {
    state.patients = value || {};
    renderAll();
  });
  subscribe("devices", (value) => {
    state.devices = value || {};
    renderAll();
  });
  subscribe("doctors", (value) => {
    state.doctors = value || {};
    renderAdminOptions();
  });
  subscribe("patientDevices", (value) => {
    state.patientDevices = value || {};
    renderAll();
  });
}

function subscribeDoctorData(uid) {
  subscribe(`doctorPatients/${uid}`, (links) => {
    clearUnsubs("scopedUnsubs");
    clearDeviceUnsubs();
    state.patients = {};
    state.devices = {};
    state.patientDevices = {};
    const patientIds = Object.keys(links || {}).filter((patientId) => links[patientId]);
    for (const patientId of patientIds) {
      subscribe(
        `patients/${patientId}`,
        (value) => {
          if (value) state.patients[patientId] = value;
          else delete state.patients[patientId];
          renderAll();
        },
        "scopedUnsubs",
      );
      subscribe(
        `patientDevices/${patientId}`,
        (value) => {
          state.patientDevices[patientId] = value || {};
          Object.keys(value || {}).forEach((deviceId) => subscribeDevice(deviceId));
          renderAll();
        },
        "scopedUnsubs",
      );
    }
    renderAll();
  });
}

function stopRealtime() {
  clearUnsubs("unsubs");
  clearUnsubs("scopedUnsubs");
  clearDeviceUnsubs();
  if (state.historyUnsub) {
    state.historyUnsub();
    state.historyUnsub = null;
  }
  state.patients = {};
  state.devices = {};
  state.doctors = {};
  state.patientDevices = {};
  state.selectedPatientId = null;
  state.selectedDeviceId = null;
}

function startRealtime() {
  stopRealtime();
  $("adminPanel").classList.toggle("hidden", state.session.role !== "admin");
  $("roleBadge").textContent = state.session.role === "admin" ? "Admin" : "Doctor";

  if (state.session.role === "admin") subscribeAdminData();
  else subscribeDoctorData(state.session.uid);

  renderAll();
}

function formData(form) {
  return Object.fromEntries(
    [...new FormData(form).entries()].map(([key, value]) => [key, typeof value === "string" ? value.trim() : value]),
  );
}

function adminMessage(text, isError = false) {
  const box = $("adminMessage");
  box.textContent = text;
  box.style.color = isError ? "var(--red)" : "var(--green)";
}

function attachAdminForm(formId, endpoint, successText) {
  $(formId).addEventListener("submit", async (event) => {
    event.preventDefault();
    const form = event.currentTarget;
    try {
      const result = await api(endpoint, formData(form));
      if (result.generatedPassword) {
        adminMessage(`${successText}. Generated device password: ${result.generatedPassword}`);
      } else {
        adminMessage(successText);
      }
      form.reset();
    } catch (err) {
      adminMessage(err.message, true);
    }
  });
}

function attachForms() {
  $("loginForm").addEventListener("submit", async (event) => {
    event.preventDefault();
    $("loginError").textContent = "";
    try {
      await signInWithEmailAndPassword(state.auth, $("loginEmail").value.trim(), $("loginPassword").value);
    } catch (err) {
      $("loginError").textContent = err.message;
    }
  });

  $("signOutButton").addEventListener("click", () => signOut(state.auth));
  attachAdminForm("doctorForm", "/api/admin/doctors", "Doctor saved");
  attachAdminForm("patientForm", "/api/admin/patients", "Patient saved");
  attachAdminForm("deviceForm", "/api/admin/devices", "Device saved");
  attachAdminForm("linkDoctorForm", "/api/admin/link-doctor-patient", "Doctor linked to patient");
  attachAdminForm("linkDeviceForm", "/api/admin/link-device-patient", "Device linked to patient");
}

async function bootstrap() {
  const configResponse = await fetch("/api/config");
  const config = await configResponse.json();
  const firebaseApp = initializeApp(config.firebase);
  state.auth = getAuth(firebaseApp);
  state.db = getDatabase(firebaseApp);

  attachForms();
  onAuthStateChanged(state.auth, async (user) => {
    if (!user) {
      stopRealtime();
      state.session = null;
      showLogin();
      setConnection("Signed out", "neutral");
      return;
    }

    try {
      state.session = await api("/api/session");
      showApp();
      setConnection("Connected", "healthy");
      startRealtime();
    } catch (err) {
      $("loginError").textContent = err.message;
      await signOut(state.auth);
    }
  });
}

bootstrap().catch((err) => {
  $("loginError").textContent = err.message;
  setConnection("Offline", "alarm");
});
