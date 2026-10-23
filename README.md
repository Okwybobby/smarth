# Smart LPG Home — Internet-Control POC

This package contains **two independent ESP32-C3 sketches** and a single consumer-facing Streamlit app. It builds on the previously reviewed sketches.

## Files
- `regulator_firebase.ino` — regulator controller. Original GPIOs, auto-open-on-boot, 25-second travel timing, gas-alarm closure, alarm latch, and HTTP `/status` + `/command` are retained. Adds Firebase remote-command polling, local execution through existing rules, and cloud acknowledgement.
- `level_sensor_firebase.ino` — the original proven L062MTW firmware, unchanged except for the previously applied API-key placeholder.
- `app.py` — Streamlit dashboard, Firebase telemetry, remote OPEN/CLOSE/STOP/RESET_ALARM, illustrative humidity (only with valid BMP280 temperature).
- `firebase_rules_poc.json` — demo-only Firebase Realtime Database rules.

## Set up
1. In `regulator_firebase.ino`, set `FIREBASE_API_KEY` to the Firebase Web API key already used in your project. Keep the existing Wi-Fi configuration as appropriate.
2. In `level_sensor_firebase.ino`, set `FIREBASE_API_KEY` to the same key. No other edits required for the level sensor.
3. In Firebase Console > Realtime Database > Rules, paste and **Publish** the content of `firebase_rules_poc.json`. The rules include the existing `lpg_sensor` and `devices/smart_regulator` paths and add `commands/*`.
4. Rename `.streamlit/secrets.toml.example` to `.streamlit/secrets.toml`, and set `FIREBASE_API_KEY`. This is a one-time local configuration, not a user-facing login. Do not commit this file to source control.
5. Run `python -m pip install -r requirements.txt`, then `python -m streamlit run app.py`.
6. Upload `regulator_firebase.ino` to ESP32-C3 #1, open Serial Monitor at 115200. Wait for `[CLOUD] Upload OK (HTTP 200)` and confirm real telemetry on Streamlit.
7. Upload `level_sensor_firebase.ino` to ESP32-C3 #2 and confirm `[Firebase] Upload OK`. If the cylinder card shows unavailable, inspect `/lpg_sensor/valid`, `/status`, and `/signal`; a powered-on ESP32 may report `no_liquid` if the ultrasonic coupling is not correct.
8. With **gas supply isolated** and motor physically observable, press Close regulator in Streamlit. Look for `[REMOTE] Queued CLOSE`, `[REMOTE] CLOSE: Closing started`, and `[REMOTE] Ack OK (HTTP 200)` in Serial Monitor. The app will display the acknowledgement. Test OPEN only under controlled bench conditions; it retains the existing firmware interlocks.

## Command protocol
The app writes `/commands/smart_regulator` as `{ "id": "unique-uuid", "action": "CLOSE", "issued_at": 1790000000 }`. The ESP32 polls it over HTTPS, rejects commands older than 20 seconds (requires NTP time sync), queues it for the main control loop, persists the last handled ID in ESP32 Preferences to avoid replay on restart, and writes `/commands/smart_regulator_ack` with `{ "id": "...", "state": "accepted|rejected", "message": "...", "timestamp": <Firebase server timestamp> }`.

A command **accepted** means the requested motor cycle was started, **not** that physical valve position was verified. Firmware continues to use timed position estimates. Cloud telemetry and commands may take several seconds due to network latency.

## Important POC limitations
- The Realtime Database rules permit **any anonymous user authenticated to this Firebase project** to write commands. There is no per-user access control. This is intentionally simplified for a closed bench POC and is **not suitable for an installed LPG system**. Remove the command rules or disable the prototype when not testing.
- The Firebase API key is configuration, not an authorization secret; these permissive rules are the actual security exposure.
- The regulator's **existing automatic opening on boot is intentionally unchanged**, as requested. Test with the gas supply isolated.
- TLS certificate verification remains disabled in ESP32 HTTPS code as in the earlier prototype. This must be changed before real deployment.
- Remote STOP and RESET_ALARM are included for POC parity with existing local commands. The local alarm detection and closing logic remain on ESP32 and do not depend on internet connectivity.
- Relative humidity is **illustrative only**, not calculated from actual moisture data. Pressure is ambient BMP280 pressure, not LPG cylinder pressure.

## Cylinder gauge update
The Streamlit interface now includes a prominent vertical cylinder graphic filled using the **real** DYP-L062MTW `level_mm` reading and the user-set reference height. It displays a visible but unfilled, grey cylinder when readings are stale, invalid, timed out, or `no_liquid`; it never fabricates a fill level. The visual is a liquid-height fraction, **not** calibrated LPG volume or remaining mass. This release changes only `app.py`; both Arduino sketches and remote-control protocol are unchanged.
