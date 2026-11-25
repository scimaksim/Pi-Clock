// Maidenhead locators from latLonToGrid in dashboard.js, run with node.
// Every result must be a valid locator (fields A-R, subsquares a-x).
const fs = require("fs");
const path = require("path");
const assert = require("assert");
const src = fs.readFileSync(path.join(__dirname, "../static/js/dashboard.js"), "utf8");
const m = src.match(/function latLonToGrid\([\s\S]*?\n}\n/);
assert(m, "latLonToGrid not found in dashboard.js");
const latLonToGrid = new Function(m[0] + "; return latLonToGrid;")();
const valid = /^[A-R]{2}[0-9]{2}[a-x]{2}$/;
const cases = [
  [35.6928, -78.7270, "FM05pq"],   // home, Lake Wheeler NC
  [0, -180, "AJ00aa"],             // antimeridian, west side
  [0, 180, "AJ00aa"],              // the same meridian written as +180
  [0, 179.99, "RJ90xa"],           // just west of the antimeridian
  [90, 0, "JR09ax"],               // north pole: top row of the grid
  [-90, 0, "JA00aa"],              // south pole
  [51.4779, -0.0015, "IO91xl"],    // Greenwich
];
let fail = 0;
for (const [lat, lon, want] of cases) {
  const got = latLonToGrid(lat, lon);
  const ok = got === want && valid.test(got);
  console.log(`${ok ? "ok  " : "FAIL"} (${lat}, ${lon}) -> ${got}${ok ? "" : ` (want ${want})`}`);
  if (!ok) fail++;
}
for (let i = 0; i < 2000; i++) {
  const lat = -90 + Math.random() * 180, lon = -200 + Math.random() * 400;
  const g = latLonToGrid(lat, lon);
  if (!valid.test(g)) { console.log(`FAIL (${lat}, ${lon}) -> ${g}`); fail++; break; }
}
process.exit(fail ? 1 : 0);
