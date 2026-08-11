#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

const char* ap_ssid = "PickleballLauncher";
const char* ap_password = "launch123";  // min 8 chars, needed for WPA2

Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver(0x40);

const int CH_SERVO = 0;
const int CH_LEFT  = 1;
const int CH_RIGHT = 2;
const int CH_DRIVE = 3;   // QuicRun 1060, drives both drive motors in parallel

const int MIN_US = 1000;   // flywheel ESC minimum throttle / arm signal
const int MAX_US = 2000;   // flywheel ESC full throttle

const int SERVO_MIN_US = 1000;  // servo's 0 degree pulse - tune once gate is mounted
const int SERVO_MAX_US = 2000;  // servo's 180 degree pulse - tune once gate is mounted
// Swapped 2026-08-11: bench test on the new ball-storage gate showed these
// were backwards (Open button was closing it and vice versa). Re-tune the
// two angle values (not which constant means what) if the gate hardware changes.
const int GATE_CLOSED_ANGLE = 160;
const int GATE_OPEN_ANGLE   = 20;

// QuicRun 1060 is a bidirectional car ESC - neutral is 1500us, NOT 1000us like the
// flywheel ESCs. Arming means holding neutral, not minimum.
const int DRIVE_NEUTRAL_US = 1500;
const int DRIVE_FORWARD_US = 1800;  // raised from 1650 after first successful test confirmed wiring/direction/calibration - still raise gradually
const unsigned long DRIVE_BURST_MS = 2000;

// Continuous drive slider: same +/-300us envelope as Drive Burst above.
// slider value -100..100 -> DRIVE_NEUTRAL_US +/- DRIVE_MAX_DELTA_US
const int DRIVE_MAX_DELTA_US = 300;

WebServer server(80);
bool armed = false;
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_TIMEOUT = 20000; // stop motors if no heartbeat for 20s

void setPulse(int channel, int us) {
  pwm.writeMicroseconds(channel, us);
}

void setServoAngle(int channel, int angle) {
  int us = map(angle, 0, 180, SERVO_MIN_US, SERVO_MAX_US);
  pwm.writeMicroseconds(channel, us);
}

const char* PAGE_HTML = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Launcher Control</title>
<style>
  * { box-sizing: border-box; }
  body {
    font-family: -apple-system, system-ui, sans-serif;
    background: #111; color: #eee;
    text-align: center; padding: 24px 16px; margin: 0;
  }
  h2 { font-weight: 600; margin-bottom: 4px; }
  .status {
    display: inline-block; padding: 6px 16px; border-radius: 20px;
    font-weight: 600; font-size: 14px; margin-bottom: 24px;
  }
  .armed { background: #1a3; color: #fff; }
  .disarmed { background: #444; color: #aaa; }
  .card {
    background: #1c1c1c; border-radius: 16px; padding: 20px;
    margin: 0 auto 16px; max-width: 400px;
  }
  .card h3 { margin: 0 0 12px; font-size: 16px; color: #aaa; font-weight: 500; }
  .pct { font-size: 32px; font-weight: 700; margin-bottom: 12px; }
  input[type=range] { width: 100%; height: 40px; -webkit-appearance: none; background: transparent; }
  input[type=range]::-webkit-slider-runnable-track { height: 8px; background: #333; border-radius: 4px; }
  input[type=range]::-webkit-slider-thumb {
    -webkit-appearance: none; width: 32px; height: 32px; border-radius: 50%;
    background: #4af; margin-top: -12px; box-shadow: 0 2px 6px rgba(0,0,0,0.4);
  }
  input[type=range]:disabled::-webkit-slider-thumb { background: #555; }
  .btnrow { max-width: 400px; margin: 0 auto 16px; display: flex; gap: 12px; }
  button { flex: 1; padding: 18px; font-size: 18px; font-weight: 700; border: none; border-radius: 12px; cursor: pointer; }
  #armBtn { background: #4af; color: #fff; }
  #stopBtn { background: #e33; color: #fff; }
  .gateBtn { background: #666; color: #fff; }
  button:active { opacity: 0.8; }
  button:disabled { background: #333; color: #777; }
</style>
</head>
<body>
  <h2>Launcher Control</h2>
  <div id="statusBadge" class="status disarmed">NOT ARMED</div>

  <div class="card">
    <h3>Right Motor</h3>
    <div class="pct" id="pctR">0%</div>
    <input type="range" id="sliderR" min="0" max="100" value="0" disabled>
  </div>

  <div class="card">
    <h3>Left Motor</h3>
    <div class="pct" id="pctL">0%</div>
    <input type="range" id="sliderL" min="0" max="100" value="0" disabled>
  </div>

  <div class="card">
    <h3>Drive Motor</h3>
    <div class="pct" id="pctD">STOP</div>
    <input type="range" id="sliderD" min="-100" max="100" value="0" disabled>
    <div style="display:flex; justify-content:space-between; font-size:12px; color:#666; margin-top:-4px;">
      <span>REVERSE</span><span>FORWARD</span>
    </div>
  </div>

  <div class="btnrow">
    <button id="armBtn" onclick="arm()">ARM</button>
    <button id="stopBtn" onclick="stopAll()">STOP</button>
  </div>

  <div class="btnrow">
    <button class="gateBtn" onclick="gateOpen()">Open Gate</button>
    <button class="gateBtn" onclick="gateClose()">Close Gate</button>
  </div>

  <div class="btnrow">
    <button class="gateBtn" onclick="driveBurst()" style="background:#a33">Drive Burst (2s)</button>
  </div>

<script>
let heartbeatTimer = null;
function startHeartbeat() {
  if (heartbeatTimer) return;
  heartbeatTimer = setInterval(() => fetch('/heartbeat'), 2000);
}
function stopHeartbeat() {
  clearInterval(heartbeatTimer);
  heartbeatTimer = null;
}
function setArmedUI(isArmed) {
  document.getElementById('statusBadge').textContent = isArmed ? 'ARMED' : 'NOT ARMED';
  document.getElementById('statusBadge').className = 'status ' + (isArmed ? 'armed' : 'disarmed');
  document.getElementById('sliderR').disabled = !isArmed;
  document.getElementById('sliderL').disabled = !isArmed;
  document.getElementById('sliderD').disabled = !isArmed;
  if (!isArmed) {
    document.getElementById('sliderR').value = 0;
    document.getElementById('sliderL').value = 0;
    document.getElementById('sliderD').value = 0;
    document.getElementById('pctR').textContent = '0%';
    document.getElementById('pctL').textContent = '0%';
    document.getElementById('pctD').textContent = 'STOP';
    stopHeartbeat();
  } else {
    startHeartbeat();
  }
}
function arm() { fetch('/arm').then(() => setArmedUI(true)); }
function stopAll() { fetch('/stop').then(() => setArmedUI(false)); }
function gateOpen() { fetch('/gateopen'); }
function gateClose() { fetch('/gateclose'); }
function driveBurst() {
  if (!confirm('Buggy will drive forward for 2 seconds. Clear path?')) return;
  fetch('/driveburst');
}
function sendSpeed() {
  const r = document.getElementById('sliderR').value;
  const l = document.getElementById('sliderL').value;
  const d = document.getElementById('sliderD').value;
  fetch(`/speed?right=${r}&left=${l}&drive=${d}`).then(res => {
    if (res.status === 403) {
      alert('Disarmed by the ESP32 (heartbeat timeout) - hit ARM again.');
      setArmedUI(false);
    }
  });
}
function driveLabel(v) {
  v = parseInt(v);
  if (v === 0) return 'STOP';
  return (v > 0 ? 'FWD ' : 'REV ') + Math.abs(v) + '%';
}
const sR = document.getElementById('sliderR');
const sL = document.getElementById('sliderL');
const sD = document.getElementById('sliderD');
sR.addEventListener('input', () => document.getElementById('pctR').textContent = sR.value + '%');
sL.addEventListener('input', () => document.getElementById('pctL').textContent = sL.value + '%');
sD.addEventListener('input', () => document.getElementById('pctD').textContent = driveLabel(sD.value));
sR.addEventListener('change', sendSpeed);
sL.addEventListener('change', sendSpeed);
sD.addEventListener('change', sendSpeed);
</script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html", PAGE_HTML);
}

void handleArm() {
  setPulse(CH_RIGHT, MIN_US);
  setPulse(CH_LEFT, MIN_US);
  setPulse(CH_DRIVE, DRIVE_NEUTRAL_US);
  armed = true;
  lastHeartbeat = millis();
  server.send(200, "text/plain", "armed");
}

void handleSpeed() {
  if (!armed) {
    server.send(403, "text/plain", "not armed");
    return;
  }
  if (server.hasArg("right")) {
    int pct = constrain(server.arg("right").toInt(), 0, 100);
    setPulse(CH_RIGHT, map(pct, 0, 100, MIN_US, MAX_US));
  }
  if (server.hasArg("left")) {
    int pct = constrain(server.arg("left").toInt(), 0, 100);
    setPulse(CH_LEFT, map(pct, 0, 100, MIN_US, MAX_US));
  }
  if (server.hasArg("drive")) {
    int val = constrain(server.arg("drive").toInt(), -100, 100);
    int us = DRIVE_NEUTRAL_US + (long)val * DRIVE_MAX_DELTA_US / 100;
    setPulse(CH_DRIVE, us);
  }
  server.send(200, "text/plain", "ok");
}

void handleStop() {
  setPulse(CH_RIGHT, MIN_US);
  setPulse(CH_LEFT, MIN_US);
  setPulse(CH_DRIVE, DRIVE_NEUTRAL_US);
  armed = false;
  server.send(200, "text/plain", "stopped");
}

void handleDriveBurst() {
  if (!armed) {
    server.send(403, "text/plain", "not armed");
    return;
  }
  Serial.println("Drive burst starting");
  setPulse(CH_DRIVE, DRIVE_FORWARD_US);
  delay(DRIVE_BURST_MS);
  setPulse(CH_DRIVE, DRIVE_NEUTRAL_US);
  Serial.println("Drive burst done, back to neutral");
  server.send(200, "text/plain", "burst complete");
}

void handleGateOpen() {
  setServoAngle(CH_SERVO, GATE_OPEN_ANGLE);
  server.send(200, "text/plain", "gate open");
}

void handleGateClose() {
  setServoAngle(CH_SERVO, GATE_CLOSED_ANGLE);
  server.send(200, "text/plain", "gate closed");
}

void handleHeartbeat() {
  lastHeartbeat = millis();
  server.send(200, "text/plain", "ok");
}

void setup() {
  Serial.begin(115200);

  Wire.begin(21, 22); // SDA, SCL - default ESP32 I2C pins
  pwm.begin();
  pwm.setPWMFreq(50); // standard RC/servo frequency

  setPulse(CH_RIGHT, MIN_US);
  setPulse(CH_LEFT, MIN_US);
  setPulse(CH_DRIVE, DRIVE_NEUTRAL_US);
  setServoAngle(CH_SERVO, GATE_CLOSED_ANGLE);

  WiFi.softAP(ap_ssid, ap_password);
  Serial.print("Access point started. Connect your phone to WiFi \"");
  Serial.print(ap_ssid);
  Serial.println("\"");
  Serial.print("Then browse to: http://");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/arm", handleArm);
  server.on("/speed", handleSpeed);
  server.on("/stop", handleStop);
  server.on("/gateopen", handleGateOpen);
  server.on("/gateclose", handleGateClose);
  server.on("/driveburst", handleDriveBurst);
  server.on("/heartbeat", handleHeartbeat);
  server.begin();
}

void loop() {
  if (armed && millis() - lastHeartbeat > HEARTBEAT_TIMEOUT) {
    Serial.println("Heartbeat lost - stopping motors");
    setPulse(CH_RIGHT, MIN_US);
    setPulse(CH_LEFT, MIN_US);
    setPulse(CH_DRIVE, DRIVE_NEUTRAL_US);
    armed = false;
  }
  server.handleClient();
}