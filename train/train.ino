#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

// Train mode: unattended demo sequencer. Set a ball count and it fires that
// many balls one at a time out of the storage net, alternating drive
// direction each shot so the buggy sweeps back and forth across the net.
// Built for the Blueprint contest demo. See flywheel_control/flywheel_control.ino
// for the manual single-shot control page this is based on.

const char* ap_ssid = "PickleballTrainer";
const char* ap_password = "launch123";  // min 8 chars, needed for WPA2

Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver(0x40);

const int CH_SERVO = 0;
const int CH_LEFT  = 1;
const int CH_RIGHT = 2;
const int CH_DRIVE = 3;   // QuicRun 1060, drives both drive motors in parallel

const int MIN_US = 1000;   // flywheel ESC minimum throttle / arm signal
const int MAX_US = 2000;   // flywheel ESC full throttle

const int SERVO_MIN_US = 1000;
const int SERVO_MAX_US = 2000;

// Same fix as flywheel_control.ino 2026-08-11: bench test on the storage
// gate showed these were backwards. Re-tune the two values if the gate
// hardware changes again.
const int GATE_CLOSED_ANGLE = 160;
const int GATE_OPEN_ANGLE   = 20;

// QuicRun 1060 is bidirectional - neutral is 1500us, not 1000us like the
// flywheel ESCs.
const int DRIVE_NEUTRAL_US   = 1500;
const int DRIVE_MAX_DELTA_US = 300;   // +/-300us envelope, same as flywheel_control.ino

// --- Train defaults - all overridable from the web UI before pressing Start ---
const int DEFAULT_BALL_COUNT     = 5;
const int DEFAULT_FLYWHEEL_PCT   = 75;    // flywheel power, 0-100
const int DEFAULT_DRIVE_PCT      = 50;    // drive power, 0-100 (of the +/-300us envelope)
const int DEFAULT_DRIVE_BURST_MS = 2500;  // how long the drive burst runs per shot - tune to net length
const int DEFAULT_GATE_OPEN_MS   = 350;   // how long the gate stays open per ball

// Balls are constantly rolling in the storage net, so the gate has to open
// and close fast enough that a second ball can't slip through behind the
// first one. Target: gate open time + the servo's own mechanical close
// speed both land comfortably under ~1000ms. Start at the default below and
// tune GATE_OPEN_MS down from the web UI if balls start doubling up; tune it
// up if the gate is closing on a ball still mid-pass.
const int GATE_CYCLE_BUDGET_MS = 1000;

// How long the drive motor sits at neutral between shots before reversing.
// This has to be long enough for the buggy to actually coast to a stop -
// reversing a moving buggy straight from one direction to the other stresses
// the drive ESC and gearbox and can make the direction change unreliable.
// 700ms is a starting guess, not a measured value - tune from the web UI
// against how much momentum the buggy actually carries.
const int DEFAULT_REVERSAL_PAUSE_MS = 700;

const unsigned long FLYWHEEL_SPINUP_MS = 500;  // let flywheels reach speed before the first shot

WebServer server(80);

enum TrainState { T_IDLE, T_SPINUP, T_FIRING, T_GAP };
TrainState trainState = T_IDLE;

int totalBalls      = DEFAULT_BALL_COUNT;
int flywheelPct     = DEFAULT_FLYWHEEL_PCT;
int drivePct        = DEFAULT_DRIVE_PCT;
int driveBurstMs    = DEFAULT_DRIVE_BURST_MS;
int gateOpenMs       = DEFAULT_GATE_OPEN_MS;
int reversalPauseMs  = DEFAULT_REVERSAL_PAUSE_MS;

int shotIndex = 0;   // 0-based index of the ball currently in flight
int direction = 1;   // 1 = forward, -1 = reverse, alternates every shot
unsigned long phaseStart = 0;
bool gateClosedThisShot = false;

bool armed = false;
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_TIMEOUT = 20000; // abort the run if the phone drops for 20s

void setPulse(int channel, int us) {
  pwm.writeMicroseconds(channel, us);
}

void setServoAngle(int channel, int angle) {
  int us = map(angle, 0, 180, SERVO_MIN_US, SERVO_MAX_US);
  pwm.writeMicroseconds(channel, us);
}

void allNeutral() {
  setPulse(CH_LEFT, MIN_US);
  setPulse(CH_RIGHT, MIN_US);
  setPulse(CH_DRIVE, DRIVE_NEUTRAL_US);
  setServoAngle(CH_SERVO, GATE_CLOSED_ANGLE);
}

void stopTrain() {
  trainState = T_IDLE;
  armed = false;
  allNeutral();
}

void beginShot() {
  // Open the gate and start the drive burst for this shot together - the
  // gate closes again partway through (see advanceTrain), the drive burst
  // keeps running past that so the buggy finishes sweeping across the net.
  setServoAngle(CH_SERVO, GATE_OPEN_ANGLE);
  int us = DRIVE_NEUTRAL_US + (long)direction * drivePct * DRIVE_MAX_DELTA_US / 100;
  setPulse(CH_DRIVE, us);
  gateClosedThisShot = false;
  trainState = T_FIRING;
  phaseStart = millis();
}

void advanceTrain() {
  if (trainState == T_IDLE) return;
  unsigned long elapsed = millis() - phaseStart;

  switch (trainState) {
    case T_SPINUP:
      if (elapsed >= FLYWHEEL_SPINUP_MS) {
        beginShot();
      }
      break;

    case T_FIRING:
      if (!gateClosedThisShot && elapsed >= (unsigned long)gateOpenMs) {
        setServoAngle(CH_SERVO, GATE_CLOSED_ANGLE);
        gateClosedThisShot = true;
      }
      if (elapsed >= (unsigned long)driveBurstMs) {
        setPulse(CH_DRIVE, DRIVE_NEUTRAL_US);
        trainState = T_GAP;
        phaseStart = millis();
      }
      break;

    case T_GAP:
      if (elapsed >= (unsigned long)reversalPauseMs) {
        shotIndex++;
        if (shotIndex >= totalBalls) {
          stopTrain();
        } else {
          direction = -direction;
          beginShot();  // flywheels are already spinning, fire immediately
        }
      }
      break;

    default:
      break;
  }
}

const char* PAGE_HTML = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Train Mode</title>
<style>
  * { box-sizing: border-box; }
  :root {
    --bg: #f6faf3;
    --surface: #ffffff;
    --border: #dbe6d4;
    --ink: #182417;
    --ink-soft: #55624f;
    --green: #1f7a41;
    --green-soft: #e5f2e5;
    --gold: #7a6a0d;
    --gold-bg: #f4efc2;
    --stop: #c0392b;
    --radius: 14px;
  }
  @media (prefers-color-scheme: dark) {
    :root {
      --bg: #11170f;
      --surface: #1a231a;
      --border: #2c3a2a;
      --ink: #eef2ea;
      --ink-soft: #a9b6a4;
      --green: #4bb573;
      --green-soft: #1f3323;
      --gold: #e0c95a;
      --gold-bg: #33301a;
      --stop: #e2564a;
    }
  }
  body {
    font-family: -apple-system, system-ui, sans-serif;
    background: var(--bg); color: var(--ink);
    padding: 24px 16px 40px; margin: 0;
  }
  .wrap { max-width: 420px; margin: 0 auto; }
  h1 {
    font-size: 20px; font-weight: 700; margin: 0 0 2px; letter-spacing: -0.01em;
  }
  .subtitle { font-size: 13px; color: var(--ink-soft); margin: 0 0 18px; }
  .status-row { display: flex; align-items: center; justify-content: space-between; margin-bottom: 16px; }
  .status {
    display: inline-flex; align-items: center; gap: 6px;
    padding: 7px 14px; border-radius: 999px;
    font-weight: 700; font-size: 13px; letter-spacing: 0.01em;
  }
  .status .dot { width: 7px; height: 7px; border-radius: 50%; background: currentColor; }
  .status.idle { background: var(--surface); color: var(--ink-soft); border: 1px solid var(--border); }
  .status.spinup, .status.gap { background: var(--gold-bg); color: var(--gold); }
  .status.firing { background: var(--green-soft); color: var(--green); }

  .progress-card {
    background: var(--surface); border: 1px solid var(--border); border-radius: var(--radius);
    padding: 16px 18px; margin-bottom: 16px;
  }
  .progress-bar { background: var(--border); border-radius: 999px; height: 10px; overflow: hidden; }
  .progress-fill { background: var(--green); height: 100%; width: 0%; transition: width 0.25s ease-out; border-radius: 999px; }
  .shotline { display: flex; justify-content: space-between; align-items: center; margin-top: 12px; }
  .shotline .count { font-size: 22px; font-weight: 700; font-variant-numeric: tabular-nums; }
  .shotline .count span { color: var(--ink-soft); font-size: 15px; font-weight: 600; }
  .dir { display: inline-flex; align-items: center; gap: 5px; font-weight: 700; font-size: 14px; }
  .dir svg { width: 16px; height: 16px; }
  .dir.fwd { color: var(--green); }
  .dir.rev { color: var(--gold); }
  .dir.none { color: var(--ink-soft); }

  .card {
    background: var(--surface); border: 1px solid var(--border); border-radius: var(--radius);
    padding: 18px; margin-bottom: 14px;
  }
  .card-title { font-size: 13px; font-weight: 700; color: var(--ink-soft); margin: 0 0 12px; }
  .field { margin-bottom: 4px; }
  .field-label { display: flex; align-items: center; gap: 8px; }
  .field-label svg { width: 16px; height: 16px; color: var(--ink-soft); flex-shrink: 0; }
  .field-row { display: flex; justify-content: space-between; align-items: center; margin-bottom: 6px; }
  .field-row label { font-size: 14px; font-weight: 600; }
  .field-row .val { font-size: 14px; color: var(--green); font-weight: 700; font-variant-numeric: tabular-nums; }

  input[type=range] {
    width: 100%; height: 32px; -webkit-appearance: none; background: transparent; margin: 2px 0 16px;
  }
  input[type=range]::-webkit-slider-runnable-track { height: 6px; background: var(--border); border-radius: 999px; }
  input[type=range]::-webkit-slider-thumb {
    -webkit-appearance: none; width: 24px; height: 24px; border-radius: 50%;
    background: var(--green); margin-top: -9px; border: 3px solid var(--surface);
    box-shadow: 0 1px 4px rgba(0,0,0,0.25);
  }
  input[type=range]:disabled::-webkit-slider-thumb { background: var(--border); }

  input[type=number] {
    width: 100%; padding: 12px; font-size: 20px; font-weight: 700; text-align: center;
    background: var(--bg); border: 1px solid var(--border); border-radius: 10px; color: var(--ink);
    font-variant-numeric: tabular-nums;
  }

  .hint { font-size: 12px; color: var(--ink-soft); margin: -10px 0 4px; }

  .btnrow { display: flex; gap: 10px; margin-top: 4px; }
  button {
    flex: 1; padding: 16px; font-size: 15px; font-weight: 700; letter-spacing: 0.02em;
    border: none; border-radius: 999px; cursor: pointer; transition: transform 0.1s ease-out;
  }
  #startBtn { background: var(--green); color: #ffffff; }
  #stopBtn { background: var(--stop); color: #ffffff; }
  button:disabled { background: var(--border); color: var(--ink-soft); }
  button:active:not(:disabled) { transform: scale(0.97); }
</style>
</head>
<body>
<div class="wrap">
  <h1>Train Mode</h1>
  <p class="subtitle">RC Ball-Launching Buggy</p>

  <div class="status-row">
    <div id="statusBadge" class="status idle"><span class="dot"></span>IDLE</div>
  </div>

  <div class="progress-card">
    <div class="progress-bar"><div class="progress-fill" id="progFill"></div></div>
    <div class="shotline">
      <div class="count"><span id="ballNum">0</span><span> / <span id="ballTotal">0</span></span></div>
      <div id="dirBadge" class="dir none">
        <svg viewBox="0 0 18 18" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M2 9h13"/><path d="M11 5l4 4-4 4"/></svg>
        <span id="dirText">--</span>
      </div>
    </div>
  </div>

  <div class="card">
    <div class="field-label" style="margin-bottom:10px;">
      <svg viewBox="0 0 18 18" fill="none" stroke="currentColor" stroke-width="1.6"><circle cx="9" cy="9" r="6.5"/><circle cx="9" cy="9" r="2.3"/></svg>
      <span class="card-title" style="margin:0;">Balls to fire</span>
    </div>
    <input type="number" id="balls" min="1" max="100" value="5">
  </div>

  <div class="card">
    <div class="field">
      <div class="field-row">
        <div class="field-label"><svg viewBox="0 0 18 18" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><path d="M3 13a6 6 0 0 1 12 0"/><path d="M9 13 12 8"/></svg><label>Flywheel power</label></div>
        <span class="val" id="fwVal">75%</span>
      </div>
      <input type="range" id="fw" min="0" max="100" value="75">
    </div>

    <div class="field">
      <div class="field-row">
        <div class="field-label"><svg viewBox="0 0 18 18" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M2 9h14"/><path d="M12 5l4 4-4 4"/><path d="M6 5 2 9l4 4"/></svg><label>Drive power</label></div>
        <span class="val" id="driveVal">50%</span>
      </div>
      <input type="range" id="drive" min="0" max="100" value="50">
    </div>

    <div class="field">
      <div class="field-row">
        <div class="field-label"><svg viewBox="0 0 18 18" fill="none" stroke="currentColor" stroke-width="1.6"><circle cx="9" cy="9" r="6.5"/><path d="M9 5.5V9l3 2"/></svg><label>Drive burst length</label></div>
        <span class="val" id="burstVal">2.5s</span>
      </div>
      <input type="range" id="burst" min="500" max="4000" step="100" value="2500">
    </div>

    <div class="field">
      <div class="field-row">
        <div class="field-label"><svg viewBox="0 0 18 18" fill="none" stroke="currentColor" stroke-width="1.6"><circle cx="9" cy="9" r="6.5"/><path d="M9 5.5V9l3 2"/></svg><label>Direction change pause</label></div>
        <span class="val" id="pauseVal">700ms</span>
      </div>
      <input type="range" id="pause" min="200" max="3000" step="50" value="700">
      <div class="hint">Time at neutral before reversing - give the buggy enough to actually stop first.</div>
    </div>

    <div class="field">
      <div class="field-row">
        <div class="field-label"><svg viewBox="0 0 18 18" fill="none" stroke="currentColor" stroke-width="1.6"><circle cx="9" cy="9" r="6.5"/><path d="M9 5.5V9l3 2"/></svg><label>Gate open time</label></div>
        <span class="val" id="gateVal">350ms</span>
      </div>
      <input type="range" id="gate" min="150" max="900" step="50" value="350">
      <div class="hint">Keep this well under 1s while tuning - balls are rolling continuously.</div>
    </div>
  </div>

  <div class="btnrow">
    <button id="startBtn" onclick="startTrain()">START TRAIN</button>
    <button id="stopBtn" onclick="stopTrain()">STOP</button>
  </div>
</div>

<script>
let heartbeatTimer = null;
let pollTimer = null;
let running = false;

function startHeartbeat() {
  if (heartbeatTimer) return;
  heartbeatTimer = setInterval(() => fetch('/heartbeat'), 2000);
}
function stopHeartbeat() {
  clearInterval(heartbeatTimer);
  heartbeatTimer = null;
}

function setConfigEnabled(enabled) {
  ['balls','fw','drive','burst','pause','gate','startBtn'].forEach(id => {
    document.getElementById(id).disabled = !enabled;
  });
}

function startTrain() {
  const balls = document.getElementById('balls').value;
  const fw = document.getElementById('fw').value;
  const drive = document.getElementById('drive').value;
  const burst = document.getElementById('burst').value;
  const pause = document.getElementById('pause').value;
  const gate = document.getElementById('gate').value;
  fetch(`/trainstart?balls=${balls}&fw=${fw}&drive=${drive}&burst=${burst}&pause=${pause}&gate=${gate}`)
    .then(() => {
      running = true;
      setConfigEnabled(false);
      startHeartbeat();
      if (!pollTimer) pollTimer = setInterval(pollStatus, 300);
    });
}

function stopTrain() {
  fetch('/trainstop').then(() => {
    running = false;
    setConfigEnabled(true);
    stopHeartbeat();
  });
}

const FWD_ARROW = '<path d="M2 9h13"/><path d="M11 5l4 4-4 4"/>';
const REV_ARROW = '<path d="M16 9H3"/><path d="M7 5 3 9l4 4"/>';

function pollStatus() {
  fetch('/trainstatus').then(r => r.json()).then(s => {
    const badge = document.getElementById('statusBadge');
    const isRunning = s.state !== 'idle';
    badge.className = 'status ' + s.state;
    badge.innerHTML = '<span class="dot"></span>' + (isRunning ? s.state.toUpperCase() : 'IDLE');

    document.getElementById('ballNum').textContent = s.ball;
    document.getElementById('ballTotal').textContent = s.total;

    const dirBadge = document.getElementById('dirBadge');
    const dirText = document.getElementById('dirText');
    const dirSvg = dirBadge.querySelector('svg');
    if (isRunning) {
      dirBadge.className = 'dir ' + (s.direction > 0 ? 'fwd' : 'rev');
      dirSvg.innerHTML = s.direction > 0 ? FWD_ARROW : REV_ARROW;
      dirText.textContent = s.direction > 0 ? 'FORWARD' : 'REVERSE';
    } else {
      dirBadge.className = 'dir none';
      dirSvg.innerHTML = FWD_ARROW;
      dirText.textContent = '--';
    }

    const pct = s.total > 0 ? Math.min(100, (s.ball / s.total) * 100) : 0;
    document.getElementById('progFill').style.width = pct + '%';

    if (!isRunning && running) {
      // run finished on its own (ball count reached) or was force-stopped
      running = false;
      setConfigEnabled(true);
      stopHeartbeat();
    }
  });
}

document.getElementById('fw').addEventListener('input', function() { document.getElementById('fwVal').textContent = this.value + '%'; });
document.getElementById('drive').addEventListener('input', function() { document.getElementById('driveVal').textContent = this.value + '%'; });
document.getElementById('burst').addEventListener('input', function() { document.getElementById('burstVal').textContent = (this.value / 1000).toFixed(1) + 's'; });
document.getElementById('pause').addEventListener('input', function() { document.getElementById('pauseVal').textContent = this.value + 'ms'; });
document.getElementById('gate').addEventListener('input', function() { document.getElementById('gateVal').textContent = this.value + 'ms'; });

pollTimer = setInterval(pollStatus, 300);
</script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html", PAGE_HTML);
}

void handleTrainStart() {
  if (server.hasArg("balls"))  totalBalls      = constrain(server.arg("balls").toInt(), 1, 100);
  if (server.hasArg("fw"))     flywheelPct     = constrain(server.arg("fw").toInt(), 0, 100);
  if (server.hasArg("drive"))  drivePct        = constrain(server.arg("drive").toInt(), 0, 100);
  if (server.hasArg("burst"))  driveBurstMs    = constrain(server.arg("burst").toInt(), 200, 8000);
  if (server.hasArg("pause"))  reversalPauseMs = constrain(server.arg("pause").toInt(), 100, 5000);
  if (server.hasArg("gate"))   gateOpenMs      = constrain(server.arg("gate").toInt(), 100, 2000);

  shotIndex = 0;
  direction = 1;
  armed = true;
  lastHeartbeat = millis();

  setServoAngle(CH_SERVO, GATE_CLOSED_ANGLE);
  setPulse(CH_DRIVE, DRIVE_NEUTRAL_US);
  setPulse(CH_LEFT,  map(flywheelPct, 0, 100, MIN_US, MAX_US));
  setPulse(CH_RIGHT, map(flywheelPct, 0, 100, MIN_US, MAX_US));

  trainState = T_SPINUP;
  phaseStart = millis();
  Serial.println("Train run started");
  server.send(200, "text/plain", "started");
}

void handleTrainStop() {
  stopTrain();
  Serial.println("Train run stopped");
  server.send(200, "text/plain", "stopped");
}

void handleTrainStatus() {
  const char* stateStr;
  switch (trainState) {
    case T_IDLE:   stateStr = "idle";   break;
    case T_SPINUP: stateStr = "spinup"; break;
    case T_FIRING: stateStr = "firing"; break;
    case T_GAP:    stateStr = "gap";    break;
    default:       stateStr = "idle";   break;
  }
  int ballDisplay = (trainState == T_IDLE) ? 0 : (shotIndex + 1);
  String json = String("{\"state\":\"") + stateStr + "\",\"ball\":" + String(ballDisplay) +
                ",\"total\":" + String(totalBalls) + ",\"direction\":" + String(direction) + "}";
  server.send(200, "application/json", json);
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

  allNeutral();

  WiFi.softAP(ap_ssid, ap_password);
  Serial.print("Access point started. Connect your phone to WiFi \"");
  Serial.print(ap_ssid);
  Serial.println("\"");
  Serial.print("Then browse to: http://");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/trainstart", handleTrainStart);
  server.on("/trainstop", handleTrainStop);
  server.on("/trainstatus", handleTrainStatus);
  server.on("/heartbeat", handleHeartbeat);
  server.begin();
}

void loop() {
  if (armed && trainState != T_IDLE && millis() - lastHeartbeat > HEARTBEAT_TIMEOUT) {
    Serial.println("Heartbeat lost - aborting train run");
    stopTrain();
  }
  advanceTrain();
  server.handleClient();
}
