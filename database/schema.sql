CREATE TABLE IF NOT EXISTS parking_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    slot INTEGER NOT NULL,
    status TEXT NOT NULL,
    distance_cm REAL,
    uptime_ms INTEGER,
    timestamp DATETIME DEFAULT (datetime('now', 'localtime'))
);

-- Node-RED forwards the 10-second heartbeat too; only store real status changes.
CREATE TRIGGER IF NOT EXISTS prevent_consecutive_duplicate_status
BEFORE INSERT ON parking_logs
FOR EACH ROW
WHEN NEW.status = (
    SELECT status
    FROM parking_logs
    WHERE slot = NEW.slot
    ORDER BY id DESC
    LIMIT 1
)
BEGIN
    SELECT RAISE(IGNORE);
END;
