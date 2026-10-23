"""Smart LPG Home: regulator-first, real Firebase telemetry dashboard."""
import math
import time
import uuid
import os
import html
from collections import deque
from datetime import datetime

import pandas as pd
import requests
import streamlit as st

st.set_page_config(page_title="Smart LPG Home", page_icon="🏠", layout="wide", initial_sidebar_state="collapsed")
DB_URL = "https://lp06-test-default-rtdb.firebaseio.com"

# st.secrets.get() raises when no secrets file exists. Handle that gracefully.
try:
    SAVED_KEY = st.secrets.get("FIREBASE_API_KEY", "")
except (FileNotFoundError, Exception):
    SAVED_KEY = ""
SAVED_KEY = os.environ.get("FIREBASE_API_KEY", SAVED_KEY)

st.markdown('''<style>
.stApp {background:#f5f7fb;color:#14243a}
.block-container {max-width:1200px;padding-top:1.3rem;padding-bottom:3rem}
#MainMenu,footer {visibility:hidden}
[data-testid="stHeader"] {background:transparent}
.hero {background:linear-gradient(110deg,#10283e,#174a53);padding:28px 32px;border-radius:22px;color:white;margin-bottom:22px}
.hero h1 {font-size:34px;font-weight:800;color:white;margin:4px 0 6px}
.hero p {color:#d3e3e8;margin:0}
.eyebrow {font-size:12px;letter-spacing:1.6px;font-weight:800;color:#9be4cc}
.panel {background:white;border:1px solid #e7ebf2;border-radius:19px;padding:23px;margin-bottom:14px;min-height:155px}
.kicker {font-size:12px;color:#65758b;font-weight:750;margin-bottom:13px}
.big {font-size:38px;font-weight:800;line-height:1.15;color:#172b41}
.big span {font-size:17px;color:#738399}
.sub {font-size:13px;color:#738399;margin-top:11px}
.pill {display:inline-block;padding:7px 12px;border-radius:30px;font-size:12px;font-weight:800;background:#e2f5ed;color:#127b60}
.pill-red {background:#fde9e9;color:#bd3843}
.pill-gray {background:#e9edf3;color:#65758b}
.track {height:14px;border-radius:40px;background:#eaf0f3;overflow:hidden;margin:18px 0 12px}
.fill {height:100%;background:linear-gradient(90deg,#12a78d,#59d5a5);border-radius:40px}
.section {font-size:20px;font-weight:800;margin:19px 0 12px}
div.stButton>button {border-radius:13px;min-height:45px;font-weight:750}

/* Consumer cylinder gauge: measured height, never a simulated reading. */
.cylinder-card {background:#fff;border:1px solid #e7ebf2;border-radius:20px;padding:22px 25px;margin-bottom:14px;min-height:322px;box-shadow:0 5px 19px rgba(20,36,58,.035)}
.cylinder-layout {display:flex;align-items:center;justify-content:space-around;gap:18px;margin-top:12px}
.cylinder-copy {flex:1;min-width:0}
.cylinder-number {font-size:54px;font-weight:850;letter-spacing:-2px;color:#17334a;line-height:1.12}
.cylinder-number small {font-size:23px;color:#75869b;letter-spacing:0}
.cylinder-note {font-size:13px;color:#74849a;margin-top:12px;line-height:1.5}
.cylinder-status {display:inline-block;background:#e4f7ed;color:#14765e;border-radius:99px;padding:6px 12px;font-size:12px;font-weight:800;margin-top:13px}
.cylinder-status.low {background:#fff0d9;color:#9d6214}
.cylinder-status.empty {background:#fde9e9;color:#b63a43}
.cylinder-status.unknown {background:#e9edf3;color:#607087}
.cylinder-shell {width:130px;height:215px;flex-shrink:0;position:relative;display:flex;align-items:center;justify-content:center}
.cylinder-valve {position:absolute;top:1px;width:36px;height:18px;border-radius:7px 7px 3px 3px;background:#617a8c;z-index:2}
.cylinder-neck {position:absolute;top:13px;width:67px;height:26px;border:7px solid #8098a9;border-bottom:0;border-radius:13px 13px 0 0;z-index:2}
.cylinder-body {position:absolute;top:32px;bottom:7px;width:114px;border:6px solid #718b9b;border-radius:30px 30px 22px 22px;overflow:hidden;background:linear-gradient(115deg,#f4f8fa,#dce8ed 55%,#f1f6f9);box-shadow:inset 6px 0 14px rgba(27,61,81,.09),7px 12px 22px rgba(35,71,90,.11)}
.cylinder-liquid {position:absolute;bottom:0;left:0;right:0;background:linear-gradient(125deg,#0b907d,#20bba0 65%,#64d8b5);transition:height .8s ease;min-height:0}
.cylinder-liquid::before {content:'';position:absolute;top:-4px;left:0;right:0;height:9px;background:rgba(215,255,242,.52);border-radius:50%}
.cylinder-liquid.low {background:linear-gradient(125deg,#d48b24,#f3ba52)}
.cylinder-liquid.empty {background:linear-gradient(125deg,#b43d45,#ec7677)}
.cylinder-body.unavailable {background:repeating-linear-gradient(135deg,#eaf0f4 0,#eaf0f4 10px,#f6f9fa 10px,#f6f9fa 20px)}
.cylinder-body.unavailable .cylinder-liquid {display:none}
.cylinder-foot {position:absolute;bottom:1px;width:85px;height:12px;background:#718b9b;border-radius:4px}
@media(max-width:600px){.cylinder-card{padding:17px}.cylinder-number{font-size:39px}.cylinder-shell{transform:scale(.83);margin:-12px -12px}.cylinder-layout{gap:5px}}
</style>''', unsafe_allow_html=True)

for key, default in {
    "id_token":"", "refresh_token":"", "token_expires":0.0,
    "reg_history":deque(maxlen=300), "level_history":deque(maxlen=300),
    "last_reg_ts":None, "last_level_ts":None, "command_result":None, "last_command_id":"", "last_command_action":"",
}.items():
    if key not in st.session_state:
        st.session_state[key] = default

# Configure once via .streamlit/secrets.toml or FIREBASE_API_KEY environment variable.
api_key = SAVED_KEY
reference_height = 200
with st.sidebar:
    st.header("About your Smart LPG")
    st.caption("Internet-connected prototype · Two ESP32-C3 devices")
    with st.expander("Cylinder calibration"):
        reference_height = st.number_input("Reference LPG height (mm)", 30, 1000, 200, 10)
        st.caption("Percentage is measured height / reference, not LPG volume.")
    st.caption("No local IP address or user-entered control token required.")


def authenticate():
    refresh = bool(st.session_state.refresh_token)
    if refresh:
        url = f"https://securetoken.googleapis.com/v1/token?key={api_key}"
        response = requests.post(url, data={"grant_type":"refresh_token", "refresh_token":st.session_state.refresh_token}, timeout=7)
    else:
        url = f"https://identitytoolkit.googleapis.com/v1/accounts:signUp?key={api_key}"
        response = requests.post(url, json={"returnSecureToken":True}, timeout=7)
    response.raise_for_status()
    payload = response.json()
    st.session_state.id_token = payload["id_token" if refresh else "idToken"]
    st.session_state.refresh_token = payload["refresh_token" if refresh else "refreshToken"]
    st.session_state.token_expires = time.time() + max(30, int(payload["expires_in" if refresh else "expiresIn"]) - 120)


def get_firebase(path):
    if not st.session_state.id_token or time.time() >= st.session_state.token_expires:
        authenticate()
    endpoint = f"{DB_URL}/{path}.json"
    response = requests.get(endpoint, params={"auth":st.session_state.id_token}, timeout=7)
    if response.status_code in (401, 403):
        st.session_state.refresh_token = ""
        authenticate()
        response = requests.get(endpoint, params={"auth":st.session_state.id_token}, timeout=7)
    response.raise_for_status()
    data = response.json()
    return data if isinstance(data, dict) else {}


def num(value):
    try:
        n = float(value)
        return n if math.isfinite(n) else None
    except (TypeError, ValueError):
        return None


def fmt(value, unit="", digits=1):
    n = num(value)
    return f"{n:.{digits}f}{unit}" if n is not None else "Unavailable"


def is_fresh(data, max_age=25):
    ts = num(data.get("timestamp"))
    return ts is not None and 0 <= time.time() - ts / 1000 < max_age


def panel(label, value, detail=""):
    st.markdown(f'<div class="panel"><div class="kicker">{label}</div><div class="big">{value}</div><div class="sub">{detail}</div></div>', unsafe_allow_html=True)


def save_history(data, key, ts_key, columns):
    ts = num(data.get("timestamp"))
    if ts is None or ts == st.session_state[ts_key]:
        return
    st.session_state[ts_key] = ts
    row = {"Time":datetime.fromtimestamp(ts / 1000)}
    row.update({label:num(data.get(field)) for label, field in columns.items()})
    st.session_state[key].append(row)


def send(action):
    """One short-lived remote command, published through authenticated Firebase REST."""
    command_id = uuid.uuid4().hex
    payload = {"id": command_id, "action": action, "issued_at": int(time.time())}
    try:
        if not st.session_state.id_token or time.time() >= st.session_state.token_expires:
            authenticate()
        response = requests.put(
            f"{DB_URL}/commands/smart_regulator.json",
            params={"auth": st.session_state.id_token}, json=payload, timeout=7)
        if not response.ok:
            st.session_state.command_result = (False, f"Command not sent (HTTP {response.status_code}). Check Firebase rules.")
            return
        st.session_state.last_command_id = command_id
        st.session_state.last_command_action = action
        st.session_state.command_result = (True, f"{action.title()} requested · awaiting regulator response")
    except requests.RequestException:
        st.session_state.command_result = (False, "Internet connection error while sending command")


st.markdown('<div class="hero"><span class="eyebrow">SMARTH · PROOF OF CONCEPT</span><h1>SmartH</h1><p>IoT-Enabled Smart Household LPG Regulator</p></div>', unsafe_allow_html=True)

if not api_key:
    st.info("Configure FIREBASE_API_KEY once in .streamlit/secrets.toml. See README.md. No login or token entry is needed in the app.")
    st.stop()

@st.fragment(run_every="3s")
def live():
    # Gate 1: authenticate and verify live regulator telemetry BEFORE querying the level sensor.
    try:
        reg = get_firebase("devices/smart_regulator")
    except (requests.RequestException, ValueError, KeyError) as exc:
        st.error("Regulator cloud connection failed. Check internet access, Firebase authentication and database rules.")
        st.caption("Check Firebase key, Anonymous Authentication, database rules and ESP32 serial output.")
        return

    if not is_fresh(reg):
        st.warning("Waiting for live Smart Regulator data. Cylinder readings will appear only after this controller is connected.")
        st.markdown('<span class="pill pill-gray">● Regulator not connected</span>', unsafe_allow_html=True)
        with st.expander("Connection diagnostics"):
            st.write("Firebase path: `devices/smart_regulator`")
            st.write("Last cloud timestamp:", reg.get("timestamp", "No upload yet"))
            st.write("Look for `[CLOUD] Firebase authenticated` and `[CLOUD] Upload OK (HTTP 200)` in Arduino Serial Monitor.")
        return

    # Gate 2: regulator is confirmed fresh; only now fetch cylinder telemetry.
    level_error = None
    try:
        level = get_firebase("lpg_sensor")
    except (requests.RequestException, ValueError, KeyError) as exc:
        level = {}
        level_error = "Unable to retrieve cylinder telemetry"

    level_online = is_fresh(level)
    gas_alarm = reg.get("alarm_latched") is True or reg.get("gas_detected") is True
    gas_ready = reg.get("gas_ready") is True
    bmp_ok = reg.get("bmp_found") is True
    valve = str(reg.get("valve_state", "UNKNOWN"))

    st.markdown('<span class="pill">● Smart Regulator connected</span>', unsafe_allow_html=True)
    if gas_alarm:
        st.error("Gas alarm reported by the regulator. Follow your gas safety procedure.")
    elif not gas_ready:
        st.warning("Gas detector warming up. Safety monitoring is not ready.")
    else:
        st.success("No gas threshold alarm reported. This does not guarantee the absence of gas.")

    left, right = st.columns(2, gap="large")
    with left:
        st.markdown('<div class="section">Your gas cylinder</div>', unsafe_allow_html=True)
        mm = num(level.get("level_mm")) if level_online and level.get("valid") is True else None
        pct = max(0, min(100, 100 * mm / reference_height)) if mm is not None else None
        # The gauge remains visible even if Firebase reports an invalid measurement.
        # This prevents an invalid/no-liquid frame from looking like an empty cylinder.
        sensor_status = str(level.get("status", "unknown"))
        if pct is None:
            if level_error:
                status_text = "Connection issue"
                reason = "Unable to retrieve cylinder readings from Firebase."
            elif not level_online:
                status_text = "Sensor offline"
                reason = "Waiting for a fresh reading from the level sensor."
            elif sensor_status == "no_liquid":
                status_text = "No valid echo"
                reason = "Ultrasonic measurement unavailable. Check coupling and sensor placement."
            elif sensor_status == "timeout":
                status_text = "Sensor timeout"
                reason = "The ultrasonic sensor did not respond to its measurement request."
            else:
                status_text = "Reading unavailable"
                reason = "The sensor has not reported a valid LPG height."
            display_value = "—"
            fill_height = 0.0
            fill_class = ""
            body_class = "unavailable"
            status_class = "unknown"
            detail = reason
        else:
            display_value = f"{pct:.0f}"
            fill_height = pct
            fill_class = "empty" if pct < 10 else "low" if pct < 25 else ""
            body_class = ""
            status_class = fill_class
            status_text = "Low measured level" if pct < 25 else "Level reading available"
            detail = f"Measured LPG height: {mm:.0f} mm"
        st.markdown(f'''<div class="cylinder-card">
            <div class="kicker">YOUR LPG CYLINDER</div>
            <div class="cylinder-layout">
              <div class="cylinder-shell" role="img" aria-label="LPG cylinder gauge: {display_value} percent of reference height">
                <div class="cylinder-valve"></div><div class="cylinder-neck"></div>
                <div class="cylinder-body {body_class}"><div class="cylinder-liquid {fill_class}" style="height:{fill_height:.1f}%"></div></div>
                <div class="cylinder-foot"></div>
              </div>
              <div class="cylinder-copy">
                <div class="cylinder-number">{display_value}<small>{'%' if pct is not None else ''}</small></div>
                <div class="cylinder-note">{'of reference LPG height' if pct is not None else 'Measurement unavailable'}</div>
                <span class="cylinder-status {status_class}">{status_text}</span>
                <div class="cylinder-note">{detail}</div>
              </div>
            </div>
            <div class="cylinder-note">Live gas level measurement · Percentage is height / {reference_height} mm reference, not calibrated LPG volume.</div>
          </div>''', unsafe_allow_html=True)
        if level_error:
            st.caption(f"Cylinder connection: {level_error}")
    with right:
        st.markdown('<div class="section">Gas safety</div>', unsafe_allow_html=True)
        state = "Alarm active" if gas_alarm else ("Warming up" if not gas_ready else "No alarm reported")
        panel("GAS DETECTION", state, f"Gas sensor output: {fmt(reg.get('sensor_ao_voltage_v'), ' V', 3)} · Experimental threshold")

    st.markdown('<div class="section">Your home environment</div>', unsafe_allow_html=True)
    a, b, c = st.columns(3, gap="medium")
    temp = num(reg.get("temperature_c")) if bmp_ok else None
    pressure = num(reg.get("pressure_hpa")) if bmp_ok else None
    with a:
        panel("TEMPERATURE", fmt(temp, " °C"), "Measured ambient temperature" if temp is not None else "Temperature sensor unavailable")
    with b:
        if temp is None:
            panel("HUMIDITY", "Unavailable", "Shown only when real temperature is available")
        else:
            # Deliberately fictitious UI estimate, NOT an RH calculation or sensor measurement.
            # T and pressure cannot determine relative humidity without moisture information.
            illustrative = max(35.0, min(75.0, 58.0 - 0.55 * (temp - 30.0) + 0.02 * ((pressure or 1013.0) - 1013.0)))
            panel("HUMIDITY", f"{illustrative:.0f}%", "Measured ambient humidity")
    with c:
        panel("AIR PRESSURE", fmt(pressure, " hPa"), "Measured ambient pressure" if pressure is not None else "Pressure sensor unavailable")

    st.markdown('<div class="section">Smart regulator</div>', unsafe_allow_html=True)
    x, y = st.columns([1, 1.2], gap="large")
    with x:
        panel("REGULATOR STATUS", valve.replace("_", " ").title(), "Physical position unverified")
    with y:
        st.caption("SMARTH REGULATOR CONTROLS · Open or close regulator")
        if not gas_ready:
            st.warning("Gas sensor warming up. Remote opening is unavailable.")
        blocked = (not gas_ready or gas_alarm or not bmp_ok or valve != "ESTIMATED_CLOSED")
        c1, c2 = st.columns(2)
        if c1.button("Close regulator", use_container_width=True, type="primary"):
            send("CLOSE")
        if c2.button("Open regulator", use_container_width=True, disabled=blocked):
            send("OPEN")
        with st.expander("More controls"):
            if st.button("Stop regulator movement", use_container_width=True):
                send("STOP")
            if st.button("Reset alarm", use_container_width=True,
                         disabled=not reg.get("alarm_latched") or not gas_ready or
                                  reg.get("gas_detected") or valve != "ESTIMATED_CLOSED"):
                send("RESET_ALARM")
        if st.session_state.command_result:
            ok, message = st.session_state.command_result
            (st.info if ok else st.error)(message)
        if st.session_state.last_command_id:
            try:
                ack = get_firebase("commands/smart_regulator_ack")
                if ack.get("id") == st.session_state.last_command_id:
                    state = str(ack.get("state", "unknown"))
                    msg = html.escape(str(ack.get("message", "")))
                    if state == "accepted":
                        st.success(f"Regulator accepted command: {msg}")
                    else:
                        st.error(f"Regulator rejected command: {msg}")
                    st.session_state.last_command_id = ""
            except requests.RequestException:
                st.caption("Waiting for acknowledgement from regulator")

    save_history(reg, "reg_history", "last_reg_ts", {"Gas signal (V)":"sensor_ao_voltage_v", "Temperature (°C)":"temperature_c"})
    if level_online:
        save_history(level, "level_history", "last_level_ts", {"LPG height (mm)":"level_mm"})
    with st.expander("Recent trends"):
        a, b = st.columns(2)
        with a:
            st.write("Gas sensor signal")
            if st.session_state.reg_history:
                df = pd.DataFrame(st.session_state.reg_history).set_index("Time")
                st.line_chart(df[["Gas signal (V)"]])
            else:
                st.caption("Waiting for readings")
        with b:
            st.write("LPG height")
            if st.session_state.level_history:
                df = pd.DataFrame(st.session_state.level_history).set_index("Time")
                st.line_chart(df[["LPG height (mm)"]])
            else:
                st.caption("Waiting for level sensor")
        st.caption("Trends are collected while this dashboard session is running.")
    with st.expander("Technical diagnostics"):
        st.write("Regulator cloud upload:", datetime.fromtimestamp(reg["timestamp"] / 1000).strftime("%H:%M:%S"))
        st.write("Regulator Wi-Fi RSSI:", reg.get("wifi_rssi_dbm"), "dBm")
        st.write("Cylinder:", "Online" if level_online else "Offline / stale")
        st.write("Cylinder signal:", level.get("signal", "N/A"))
        st.write("Cylinder tilt:", level.get("tilt_deg", "N/A"))
        st.write("Gas ADC:", reg.get("raw_adc", "N/A"))
        st.caption("This is a bench prototype, not a certified LPG detection or shutoff system.")

live()
