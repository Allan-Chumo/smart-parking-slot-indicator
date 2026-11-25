// Unit tests for the JavaScript inside the Node-RED function nodes.
// Run with: node --test tests/unit
const { test } = require("node:test");
const assert = require("node:assert");
const path = require("node:path");

const flows = require(path.join(__dirname, "..", "..", "node-red", "flows.json"));
const byName = (name) => flows.find((n) => n.name === name);

function makeFunction(name) {
  const node = byName(name);
  assert.ok(node, `function node "${name}" not found`);
  const store = {};
  const flow = { get: (k) => store[k], set: (k, v) => { store[k] = v; } };
  const fn = new Function("msg", "flow", node.func);
  return (payload) => fn({ payload: structuredClone(payload) }, flow);
}

const status = (slot, s, distance = 8.5, uptime = 12000) =>
  ({ slot, status: s, distance_cm: distance, uptime_ms: uptime });

test("Prepare Live Parking Log builds an insert for a valid status", () => {
  const prepare = makeFunction("Prepare Live Parking Log");
  const msg = prepare(status(2, "OCCUPIED", 6.04, 4000.9));
  assert.match(msg.topic, /^INSERT INTO parking_logs/);
  assert.match(msg.topic, /VALUES \(2, 'OCCUPIED', 6\.04, 4000, datetime\('now', 'localtime'\)\);$/);
});

test("Prepare Live Parking Log skips repeated statuses but logs changes", () => {
  const prepare = makeFunction("Prepare Live Parking Log");
  assert.ok(prepare(status(1, "AVAILABLE")));
  assert.strictEqual(prepare(status(1, "AVAILABLE", 30, 20000)), null);
  assert.ok(prepare(status(2, "AVAILABLE")));
  assert.ok(prepare(status(1, "OCCUPIED")));
});

test("Prepare Live Parking Log rejects invalid messages", () => {
  const prepare = makeFunction("Prepare Live Parking Log");
  for (const bad of [
    null,
    status(0, "OCCUPIED"),
    status(4, "OCCUPIED"),
    status(1, "SENSOR_ERROR"),
    status(1, "UNKNOWN"),
    status(1, "OCCUPIED'); DROP TABLE parking_logs; --"),
    status(1, "OCCUPIED", "abc"),
    { slot: 1, status: "OCCUPIED", distance_cm: 5 },
  ]) {
    assert.strictEqual(prepare(bad), null, JSON.stringify(bad));
  }
});

test("Parking Summary counts available and occupied slots", () => {
  const summary = makeFunction("Parking Summary");
  summary(status(1, "OCCUPIED"));
  const msg = summary(status(2, "AVAILABLE"));
  assert.strictEqual(msg.available, 1);
  assert.strictEqual(msg.occupied, 1);
  assert.match(msg.payload, /Unknown: 1/);
  assert.doesNotMatch(msg.payload, /Alert/);
});

test("Parking Summary alerts when all slots are occupied", () => {
  const summary = makeFunction("Parking Summary");
  [1, 2, 3].forEach((s) => summary(status(s, "OCCUPIED")));
  const msg = summary(status(3, "OCCUPIED"));
  assert.strictEqual(msg.occupied, 3);
  assert.match(msg.payload, /All parking slots occupied/);
});

test("Parking Summary ignores unknown slots", () => {
  assert.strictEqual(makeFunction("Parking Summary")(status(9, "OCCUPIED")), null);
});

for (const slot of [1, 2, 3]) {
  test(`Slot ${slot} colour follows the status`, () => {
    const colour = (s) => {
      const fn = new Function("msg", byName(`Set Slot ${slot} Status Colour`).func);
      return fn({ payload: s }).ui_update.color;
    };
    assert.strictEqual(colour("OCCUPIED"), "#e53935");
    assert.strictEqual(colour("AVAILABLE"), "#2e7d32");
    assert.strictEqual(colour("SENSOR_ERROR"), "#717171");
  });
}

test("Dashboard count widgets show the counts, not the summary text", () => {
  assert.strictEqual(byName("Available slots display").format, "{{msg.available}}");
  assert.strictEqual(byName("Occupied slots display").format, "{{msg.occupied}}");
});

test("Flow subscribes to all three slot status topics", () => {
  const topics = flows.filter((n) => n.type === "mqtt in").map((n) => n.topic).sort();
  assert.deepStrictEqual(topics, [1, 2, 3].map((s) => `smartparking/slot${s}/status`));
});

test("Database path is not tied to one computer", () => {
  const db = flows.find((n) => n.type === "sqlitedb").db;
  assert.ok(!/^[A-Za-z]:\\|^\//.test(db), db);
});
