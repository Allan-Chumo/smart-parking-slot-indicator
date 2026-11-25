# Smart Parking Slot Indicator

An IoT prototype that detects whether each of three parking slots is free or occupied, lights a green or red LED per slot, and reports the status to a live dashboard and a database for analytics.

```
HC-SR04 sensors -> ESP32 (Wokwi) --MQTT--> broker.emqx.io --> Node-RED --> SQLite
                   green/red LEDs                               |
                                                                +--> Dashboard (/dashboard/page1)
```

- **Live simulation:** https://wokwi.com/projects/481115472602800129
- **Occupied threshold:** a slot is OCCUPIED when the distance is below **15 cm** for 3 consecutive readings (about 0.75 s).

## Repository layout

| Path | Contents |
|---|---|
| `sketch/` | ESP32 firmware (`sketch.ino`), Wokwi wiring (`diagram.json`), libraries (`libraries.txt`), Wokwi CLI config (`wokwi.toml`) |
| `node-red/flows.json` | Node-RED flow: MQTT inputs, dashboard, SQLite logging |
| `database/schema.sql` | `parking_logs` table and the duplicate-status trigger |
| `database/seed.sql` | Data recorded during test runs on 2026-11-21 (cleaned) |
| `docs/` | Analytics report, database notes, screenshots |
| `tests/unit/` | Database tests (Python) and Node-RED function tests (JavaScript) |
| `tests/integration/` | System integration tests: real MQTT broker + Node-RED + SQLite in Docker |
| `tests/wokwi/` | Firmware tests that drive the simulated sensors and check LEDs |

## Running the system

### 1. ESP32 in Wokwi

Open the project link above and press **Start**. To recreate it from this repo: create a new ESP32 project on wokwi.com and paste in `sketch/sketch.ino`, `sketch/diagram.json` and `sketch/libraries.txt`.

While the simulation runs, click a sensor and drag its distance slider to simulate a car (below 15 cm = occupied).

### 2. Database

```sh
sqlite3 parking.db < database/schema.sql
sqlite3 parking.db < database/seed.sql   # optional: sample data
```

### 3. Node-RED

Requires Node-RED 4 with `@flowfuse/node-red-dashboard@1.37.0` and `node-red-node-sqlite@2.0.1` (install from **Manage palette**).

1. **Import** `node-red/flows.json`.
2. Open the **SQLite database** config node and set the path to the full location of your `parking.db`.
3. **Deploy**, then open http://localhost:1880/dashboard/page1.

## MQTT topics

| Topic | Direction | Payload |
|---|---|---|
| `smartparking/slotN/status` | ESP32 -> | `{"slot":1,"status":"OCCUPIED","distance_cm":8.5,"uptime_ms":12000}` (retained, every change + every 10 s) |
| `smartparking/slotN/event` | ESP32 -> | `{"slot":1,"from":"AVAILABLE","to":"OCCUPIED",...}` (on change) |
| `smartparking/system/connection` | ESP32 -> | `ONLINE` / `OFFLINE` (last will) |
| `smartparking/command` | -> ESP32 | `REFRESH` or `STATUS` republishes all slots |

Status values: `AVAILABLE`, `OCCUPIED`, `SENSOR_ERROR`, `UNKNOWN`. Only `AVAILABLE` and `OCCUPIED` changes are stored in the database.

## Testing

| Level | What is tested | How to run |
|---|---|---|
| Unit: database | Schema, duplicate-status trigger, seed data, report queries | `pytest tests/unit` |
| Unit: Node-RED | Each function node: validation, SQL building, summary counts, colours, dashboard config | `node --test tests/unit` |
| System integration | MQTT message -> Node-RED -> row in SQLite; duplicates and invalid/malicious messages rejected; dashboard served | `pytest tests/integration` (needs Docker) |
| Firmware simulation | Every slot goes AVAILABLE -> OCCUPIED -> AVAILABLE with correct LEDs; 15 cm boundary; brief readings ignored | Wokwi CLI (see below) |

Setup for the Python tests:

```sh
python3 -m venv .venv
.venv/bin/pip install -r tests/requirements.txt
```

The integration tests start a private Mosquitto broker and Node-RED (with the real `flows.json`) in Docker, so they never touch the public broker.

### Firmware simulation tests

```sh
arduino-cli compile --fqbn esp32:esp32:esp32 --output-dir sketch/build sketch
wokwi-cli --timeout 120000 --scenario tests/wokwi/slot_detection.test.yaml sketch
```

`wokwi-cli` needs a token from https://wokwi.com/dashboard/ci in the `WOKWI_CLI_TOKEN` environment variable.

### Continuous integration

`.github/workflows/ci.yml` runs all four levels on every push and pull request. Add `WOKWI_CLI_TOKEN` under **Settings -> Secrets and variables -> Actions** to enable the firmware simulation; without it the firmware is still compiled and the simulation step is skipped with a warning.

### Manual end-to-end check

Automated tests cover each link of the chain; this checklist confirms the full chain with the public broker:

1. Start the Wokwi simulation; serial shows `Wi-Fi CONNECTED!` and `Attempting MQTT connection... connected`.
2. In Node-RED, the MQTT input nodes show **connected**.
3. Set slot 1 to 10 cm: red LED on, dashboard shows slot 1 OCCUPIED in red, Occupied count increases.
4. `SELECT * FROM parking_logs ORDER BY id DESC LIMIT 1;` shows the new OCCUPIED row.
5. Set all three slots below 15 cm: dashboard shows the "All parking slots occupied" alert.
6. Stop the simulation: `smartparking/system/connection` becomes `OFFLINE` (visible in an MQTT client).

## Known limitations

- The public broker `broker.emqx.io` has no authentication. Anyone can publish to the `smartparking/...` topics, so use a private broker for anything beyond a classroom demo.
- Readings close to 15 cm can flip a slot back and forth; the 3-reading confirmation reduces but does not remove this.
- `seed.sql` rows from id 64 onwards were recorded while the firmware used a 50 cm threshold.
- Timestamps are the Node-RED computer's local time.
