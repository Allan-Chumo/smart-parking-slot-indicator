"""System integration tests: MQTT -> Node-RED -> SQLite, plus the dashboard."""
import requests

from conftest import NODE_RED_URL, wait_for


def status(slot, state, distance=8.5, uptime=12000):
    return {"slot": slot, "status": state, "distance_cm": distance, "uptime_ms": uptime}


def rows(db):
    result = db("SELECT slot, status, distance_cm, uptime_ms FROM parking_logs ORDER BY id;")
    return [(r["slot"], r["status"], r["distance_cm"], r["uptime_ms"]) for r in result]


def wait_for_rows(db, count):
    try:
        wait_for(lambda: len(rows(db)) == count, timeout=10, message=f"{count} rows")
    except TimeoutError:
        raise AssertionError(f"expected {count} rows, got {rows(db)}")
    return rows(db)


def test_dashboard_page_is_served(stack):
    response = requests.get(f"{NODE_RED_URL}/dashboard/page1", timeout=5)
    assert response.status_code == 200


def test_status_message_is_stored(db, publish):
    publish(1, status(1, "OCCUPIED", 8.5, 12000))

    assert wait_for_rows(db, 1) == [(1, "OCCUPIED", 8.5, 12000)]
    [row] = db("SELECT timestamp FROM parking_logs;")
    assert row["timestamp"]


def test_heartbeat_repeats_are_not_stored(db, publish):
    for uptime in (10000, 20000, 30000):
        publish(2, status(2, "AVAILABLE", 28.1, uptime))
    publish(2, status(2, "OCCUPIED", 6.0, 40000))

    assert wait_for_rows(db, 2) == [(2, "AVAILABLE", 28.1, 10000), (2, "OCCUPIED", 6.0, 40000)]


def test_each_slot_is_logged_independently(db, publish):
    for slot in (1, 2, 3):
        publish(slot, status(slot, "AVAILABLE", 30.0, slot))

    assert sorted(wait_for_rows(db, 3)) == [(1, "AVAILABLE", 30.0, 1), (2, "AVAILABLE", 30.0, 2), (3, "AVAILABLE", 30.0, 3)]


def test_invalid_messages_are_rejected(db, publish):
    publish(3, status(3, "SENSOR_ERROR", -1))
    publish(3, status(3, "UNKNOWN", -1))
    publish(3, status(4, "OCCUPIED"))
    publish(3, {"slot": 3, "status": "OCCUPIED", "distance_cm": "abc", "uptime_ms": 1})
    publish(3, status(3, "OCCUPIED'); DROP TABLE parking_logs; --"))
    publish(3, "not json")
    # Messages on one topic are delivered in order, so once this arrives the rest were handled.
    publish(3, status(3, "AVAILABLE", 99.0, 5))

    assert wait_for_rows(db, 1) == [(3, "AVAILABLE", 99.0, 5)]
