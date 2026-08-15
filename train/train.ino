#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

// Train mode: unattended demo sequencer.
//
// Mechanism (rewritten 2026-08-14 - the buggy is fully stopped for every
// shot, it does not fire while moving):
//   SHOOT (gate opens, buggy stays neutral) -> cooldown (gate closes) ->
//   DRIVE one segment -> cooldown (buggy comes to a full stop) -> SHOOT ...
// "Balls per pass" shots are spaced across one full net length INCLUDING
// both ends - 2 per pass means one shot at the near end and one at the far
// end, 4 per pass means near end, 1/3, 2/3, far end. That means the last
// shot of a pass already sits at the far end, so no extra drive is needed
// before the direction flips and the next pass's first shot fires right
// there. A pass of exactly 1 ball is the one exception: there's no second
// endpoint to space against, so it still drives the full length after its
// single shot before flipping. Flywheels spin up once at the start and
// stay spinning through the entire run - they are always up to speed
// before the gate opens for any shot.
//
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

// --- Primary train defaults - shown on the main screen ---
const int DEFAULT_BALL_COUNT    = 4;     // total balls fired across the whole run
const int DEFAULT_SHOTS_PER_LEG = 4;     // balls fired per one-way pass across the net
                                          // (4 balls + 4 per pass = one pass, no reversal, by default)
const int DEFAULT_FLYWHEEL_PCT  = 75;    // flywheel power, 0-100
const int DEFAULT_DRIVE_PCT     = 60;    // drive power, 0-100 (of the +/-300us envelope)

// Time to drive the FULL net length at the configured drive power, in ms.
// This is not measured yet - it's a placeholder. Bench test it: run Drive
// Power at a fixed value for a few seconds, see how far the buggy actually
// travels, then scale the time up or down until it covers the real net
// length, and set this to that value. Each shot's actual drive segment is
// this divided by "balls per pass," computed automatically.
const int DEFAULT_TRAVERSE_MS = 6000;

// Fixed, not user-adjustable - both tested well and didn't need tuning,
// so they're not worth sliders on screen.
const int GATE_OPEN_MS        = 500;  // how long the gate stays open per shot
const int POST_SHOOT_PAUSE_MS = 300;  // gate finishes closing before the drive segment starts

// --- Advanced defaults - tucked under the Advanced Settings disclosure ---
const int DEFAULT_POST_DRIVE_PAUSE_MS  = 500;  // buggy comes to a full stop before the next gate opens

// Working theory from a real field test (2026-08-15): the QuicRun 1060
// didn't reverse for the pass-2 return leg, plausibly because bidirectional
// ESCs commonly treat a reverse command arriving right after forward as a
// brake pulse rather than an actual reverse, unless the signal explicitly
// holds at neutral for a moment first. This is that explicit hold,
// inserted only on the first drive segment after a direction flip - not
// confirmed against the QuicRun 1060 datasheet, tune if it's still wrong.
const int DEFAULT_DIR_SETTLE_MS = 500;

// Balls are constantly rolling in the storage net, so the gate has to open
// and close fast enough that a second ball can't slip through behind the
// first one. Keep gate open time + the post-shoot pause comfortably under
// ~1000ms combined - re-measure both if the gate hardware changes.
const int GATE_CYCLE_BUDGET_MS = 1000;

const unsigned long FLYWHEEL_SPINUP_MS = 500;  // let flywheels reach speed before the first shot

WebServer server(80);

enum TrainState { T_IDLE, T_SPINUP, T_SHOOT, T_POST_SHOOT, T_DRIVE, T_POST_DRIVE, T_DIR_SETTLE, T_TEST_DRIVE, T_TEST_PAUSE };
TrainState trainState = T_IDLE;

int totalBalls        = DEFAULT_BALL_COUNT;
int shotsPerLeg        = DEFAULT_SHOTS_PER_LEG;
int flywheelPct        = DEFAULT_FLYWHEEL_PCT;
int drivePct           = DEFAULT_DRIVE_PCT;
int traverseMs         = DEFAULT_TRAVERSE_MS;
int postDrivePauseMs    = DEFAULT_POST_DRIVE_PAUSE_MS;
int dirSettleMs         = DEFAULT_DIR_SETTLE_MS;
// 0 = both flywheels equal, 1 = "spin right" (right motor full, left motor
// 50 points slower), -1 = "spin left" (mirrored). Which physical direction
// the ball actually curves toward is unverified - test both and see, flip
// setFlywheels() below if it's backwards from the button label.
int spinMode = 0;

int shotIndex   = 0;   // total shots fired so far, 0-based
int legShotCount = 0;  // shots fired within the current pass, resets at each leg boundary
int direction   = 1;   // 1 = forward, -1 = reverse, flips once a full pass completes
bool driveEndsLeg = false;  // set when the upcoming drive segment is the one that completes the pass (only true for balls-per-pass == 1)
bool directionJustFlipped = false;  // consumed by the next drive segment to decide whether it needs the neutral settle first
int testSegmentsRemaining = 0;  // counts down during a traverse test, mirrors a real leg's segment count
unsigned long phaseStart = 0;

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

// Applies flywheelPct to both motors, or a 50-point differential between
// them if spinMode is set, to put spin on the ball.
void setFlywheels() {
  int slowPct = flywheelPct - 50;
  if (slowPct < 0) slowPct = 0;

  int leftPct  = flywheelPct;
  int rightPct = flywheelPct;
  if (spinMode > 0)      { leftPct = slowPct; }       // spin right: right motor full, left slower
  else if (spinMode < 0) { rightPct = slowPct; }       // spin left: left motor full, right slower

  setPulse(CH_LEFT,  map(leftPct,  0, 100, MIN_US, MAX_US));
  setPulse(CH_RIGHT, map(rightPct, 0, 100, MIN_US, MAX_US));
}

void stopTrain() {
  trainState = T_IDLE;
  armed = false;
  allNeutral();
}

void beginShoot() {
  // Drive stays neutral through the whole shoot phase - the buggy is
  // stopped for every shot, it never fires while moving.
  setServoAngle(CH_SERVO, GATE_OPEN_ANGLE);
  trainState = T_SHOOT;
  phaseStart = millis();
}

void beginTestSegment() {
  int us = DRIVE_NEUTRAL_US + (long)drivePct * DRIVE_MAX_DELTA_US / 100;  // test always drives forward
  setPulse(CH_DRIVE, us);
  trainState = T_TEST_DRIVE;
  phaseStart = millis();
}

// Balls-per-pass shots include both endpoints of the net length, so an
// N-ball pass only needs N-1 drive segments between them (2 balls = 1
// segment spanning the whole length, 4 balls = 3 segments of a third each).
// The one exception is a 1-ball pass, which has no second endpoint to
// space against - it still needs exactly one full-length segment to reach
// the far end before the next pass can start there.
int segmentsPerLeg() {
  int perLeg = shotsPerLeg < 1 ? 1 : shotsPerLeg;
  return (perLeg >= 2) ? (perLeg - 1) : 1;
}

int segmentDriveMs() {
  return traverseMs / segmentsPerLeg();
}

void advanceTrain() {
  if (trainState == T_IDLE) return;
  unsigned long elapsed = millis() - phaseStart;

  switch (trainState) {
    case T_SPINUP:
      // Flywheels have been spinning since handleTrainStart() - this is
      // just the settle time before the very first shot of the run.
      if (elapsed >= FLYWHEEL_SPINUP_MS) {
        beginShoot();
      }
      break;

    case T_SHOOT:
      if (elapsed >= (unsigned long)GATE_OPEN_MS) {
        setServoAngle(CH_SERVO, GATE_CLOSED_ANGLE);
        trainState = T_POST_SHOOT;
        phaseStart = millis();
      }
      break;

    case T_POST_SHOOT:
      if (elapsed >= (unsigned long)POST_SHOOT_PAUSE_MS) {
        shotIndex++;
        legShotCount++;
        Serial.print("Shot fired: #"); Serial.print(shotIndex);
        Serial.print(" of "); Serial.print(totalBalls);
        Serial.print(" dir="); Serial.println(direction);
        if (shotIndex >= totalBalls) {
          stopTrain();  // that was the last ball, no more driving needed
          break;
        }

        bool passComplete = (legShotCount >= shotsPerLeg);
        if (passComplete && shotsPerLeg >= 2) {
          // The shot spacing already carried the buggy to the far end -
          // flip and fire the next pass's first shot right here, no drive.
          direction = -direction;
          legShotCount = 0;
          directionJustFlipped = true;
          Serial.println("Pass complete - reversing direction");
          beginShoot();
        } else if (directionJustFlipped) {
          // First drive segment since a direction flip - hold neutral
          // explicitly before reversing, see DEFAULT_DIR_SETTLE_MS.
          driveEndsLeg = passComplete;
          directionJustFlipped = false;
          setPulse(CH_DRIVE, DRIVE_NEUTRAL_US);
          trainState = T_DIR_SETTLE;
          phaseStart = millis();
        } else {
          // Either more shots remain in this pass, or (balls-per-pass == 1)
          // this is the single trailing drive that completes the pass.
          driveEndsLeg = passComplete;
          int us = DRIVE_NEUTRAL_US + (long)direction * drivePct * DRIVE_MAX_DELTA_US / 100;
          setPulse(CH_DRIVE, us);
          trainState = T_DRIVE;
          phaseStart = millis();
        }
      }
      break;

    case T_DIR_SETTLE:
      if (elapsed >= (unsigned long)dirSettleMs) {
        int us = DRIVE_NEUTRAL_US + (long)direction * drivePct * DRIVE_MAX_DELTA_US / 100;
        setPulse(CH_DRIVE, us);
        trainState = T_DRIVE;
        phaseStart = millis();
      }
      break;

    case T_DRIVE:
      if (elapsed >= (unsigned long)segmentDriveMs()) {
        setPulse(CH_DRIVE, DRIVE_NEUTRAL_US);
        trainState = T_POST_DRIVE;
        phaseStart = millis();
      }
      break;

    case T_POST_DRIVE:
      if (elapsed >= (unsigned long)postDrivePauseMs) {
        if (driveEndsLeg) {
          // Only reached by a balls-per-pass == 1 pass - the drive just
          // finished carrying the buggy to the far end.
          direction = -direction;
          legShotCount = 0;
          driveEndsLeg = false;
          directionJustFlipped = true;
          Serial.println("Pass complete - reversing direction");
        }
        beginShoot();
      }
      break;

    case T_TEST_DRIVE:
      // Drive-only calibration run - no gate, no flywheels. Runs the same
      // number of stop-start segments as a real pass (see "balls per pass")
      // instead of one continuous burst, because repeated starts from a
      // dead stop cover noticeably less distance per second of drive time
      // than one continuous run at the same total time - a continuous test
      // would calibrate a Traverse Time that then falls short in the real,
      // segmented run.
      if (elapsed >= (unsigned long)segmentDriveMs()) {
        setPulse(CH_DRIVE, DRIVE_NEUTRAL_US);
        testSegmentsRemaining--;
        if (testSegmentsRemaining <= 0) {
          trainState = T_IDLE;
          armed = false;
          Serial.println("Traverse test complete");
        } else {
          trainState = T_TEST_PAUSE;
          phaseStart = millis();
        }
      }
      break;

    case T_TEST_PAUSE:
      if (elapsed >= (unsigned long)postDrivePauseMs) {
        beginTestSegment();
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
  .subtitle { font-size: 13px; color: var(--ink-soft); margin: 0; }
  .header-row { display: flex; align-items: flex-start; justify-content: space-between; gap: 12px; margin-bottom: 18px; }
  .status {
    display: inline-flex; align-items: center; gap: 6px;
    padding: 7px 14px; border-radius: 999px;
    font-weight: 700; font-size: 13px; letter-spacing: 0.01em;
  }
  .status .dot { width: 7px; height: 7px; border-radius: 50%; background: currentColor; }
  .status.idle { background: var(--surface); color: var(--ink-soft); border: 1px solid var(--border); }
  .status.pending { background: var(--gold-bg); color: var(--gold); }
  .status.active { background: var(--green-soft); color: var(--green); }

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

  .two-col { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-bottom: 14px; }

  .spin-row { display: flex; gap: 8px; margin-bottom: 4px; }
  .spin-opt {
    flex: 1; padding: 10px 4px; font-size: 12.5px; font-weight: 700; letter-spacing: 0;
    border-radius: 999px; border: 1.5px solid var(--border); background: var(--surface);
    color: var(--ink-soft); cursor: pointer; transition: transform 0.1s ease-out;
  }
  .spin-opt.selected { background: var(--green); color: #ffffff; border-color: var(--green); }
  .spin-opt:disabled { background: var(--surface); color: var(--border); border-color: var(--border); cursor: default; }
  .spin-opt:active:not(:disabled) { transform: scale(0.96); }

  details.card summary {
    list-style: none; cursor: pointer; display: flex; align-items: center;
    justify-content: space-between; font-size: 13px; font-weight: 700; color: var(--ink-soft);
  }
  details.card summary::-webkit-details-marker { display: none; }
  details.card summary .chev { transition: transform 0.15s ease-out; }
  details.card[open] summary .chev { transform: rotate(90deg); }
  details.card summary::after { content: ''; }
  details.card .adv-body { margin-top: 14px; }

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
  <div class="header-row">
    <div>
      <h1>Train Mode</h1>
      <p class="subtitle">RC Ball-Launching Buggy</p>
    </div>
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
    <div class="two-col">
      <div>
        <div class="field-label" style="margin-bottom:10px;">
          <svg viewBox="0 0 18 18" fill="none" stroke="currentColor" stroke-width="1.6"><circle cx="9" cy="9" r="6.5"/><circle cx="9" cy="9" r="2.3"/></svg>
          <span class="card-title" style="margin:0;">Balls to fire</span>
        </div>
        <input type="number" id="balls" min="1" max="100" value="4">
      </div>
      <div>
        <div class="field-label" style="margin-bottom:10px;">
          <svg viewBox="0 0 18 18" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><path d="M4 15V3"/><path d="M4 4h9l-2 3 2 3H4"/></svg>
          <span class="card-title" style="margin:0;">Balls per pass</span>
        </div>
        <input type="number" id="perleg" min="1" max="50" value="4">
      </div>
    </div>
    <div class="hint">Balls per pass = shots across one direction, including both ends - 2 means one at each end of the net. Set it equal to "balls to fire" for a single one-way run.</div>
  </div>

  <div class="card">
    <div class="field">
      <div class="field-row">
        <div class="field-label"><svg viewBox="0 0 18 18" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"><path d="M3 13a6 6 0 0 1 12 0"/><path d="M9 13 12 8"/></svg><label>Flywheel power</label></div>
        <span class="val" id="fwVal">75%</span>
      </div>
      <input type="range" id="fw" min="0" max="100" value="75">
      <div class="spin-row">
        <button type="button" class="spin-opt selected" data-spin="0" onclick="setSpin(0,this)">Straight</button>
        <button type="button" class="spin-opt" data-spin="1" onclick="setSpin(1,this)">Spin right</button>
        <button type="button" class="spin-opt" data-spin="-1" onclick="setSpin(-1,this)">Spin left</button>
      </div>
    </div>

    <div class="field">
      <div class="field-row">
        <div class="field-label"><svg viewBox="0 0 18 18" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M2 9h14"/><path d="M12 5l4 4-4 4"/><path d="M6 5 2 9l4 4"/></svg><label>Drive power</label></div>
        <span class="val" id="driveVal">60%</span>
      </div>
      <input type="range" id="drive" min="0" max="100" value="60">
    </div>

    <div class="field">
      <div class="field-row">
        <div class="field-label"><svg viewBox="0 0 18 18" fill="none" stroke="currentColor" stroke-width="1.6"><circle cx="9" cy="9" r="6.5"/><path d="M9 5.5V9l3 2"/></svg><label>Traverse time (full net length)</label></div>
        <span class="val" id="traverseVal">6.0s</span>
      </div>
      <input type="range" id="traverse" min="500" max="8000" step="100" value="6000">
      <div class="hint">Not measured yet - bench test at this drive power and set this to how long a full end-to-end run actually takes.</div>
    </div>
  </div>

  <details class="card">
    <summary>Advanced settings <span class="chev">&#8250;</span></summary>
    <div class="adv-body">
      <div class="field" style="padding-bottom:14px; margin-bottom:14px; border-bottom:1px solid var(--border);">
        <div class="field-label" style="margin-bottom:8px;">
          <svg viewBox="0 0 18 18" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M2 9h14"/><path d="M12 5l4 4-4 4"/></svg>
          <label>Traverse test</label>
        </div>
        <div class="hint" style="margin-top:0;">Runs the same number of stop-start segments as "balls per pass," at the Drive Power and Traverse Time set above - no gate, no flywheels. This matches the real run's repeated-stop behavior, so it may cover less distance than one continuous drive would. Watch how far it actually goes, adjust the two sliders above, run it again - once it covers the real net length you're calibrated, and every shot uses that same speed and time to work out where it is.</div>
        <button id="testDriveBtn" type="button" onclick="testDrive()" style="width:100%; margin-top:10px; background:var(--surface); color:var(--green); border:1.5px solid var(--green);">RUN TRAVERSE TEST</button>
      </div>

      <div class="field">
        <div class="field-row">
          <div class="field-label"><svg viewBox="0 0 18 18" fill="none" stroke="currentColor" stroke-width="1.6"><circle cx="9" cy="9" r="6.5"/><path d="M9 5.5V9l3 2"/></svg><label>Post-drive pause</label></div>
          <span class="val" id="pdriveVal">500ms</span>
        </div>
        <input type="range" id="pdrive" min="100" max="5000" step="50" value="500">
        <div class="hint">Lets the buggy come to a full stop before the next gate opens.</div>
      </div>

      <div class="field" style="margin-bottom:0;">
        <div class="field-row">
          <div class="field-label"><svg viewBox="0 0 18 18" fill="none" stroke="currentColor" stroke-width="1.6"><circle cx="9" cy="9" r="6.5"/><path d="M9 5.5V9l3 2"/></svg><label>Direction settle</label></div>
          <span class="val" id="dirsettleVal">500ms</span>
        </div>
        <input type="range" id="dirsettle" min="100" max="3000" step="50" value="500">
        <div class="hint">Explicit neutral hold before the first drive segment in a new direction - some ESCs treat a reverse command right after forward as a brake unless it sees neutral first.</div>
      </div>
    </div>
  </details>

  <div class="btnrow">
    <button id="startBtn" onclick="startTrain()">START TRAIN</button>
    <button id="stopBtn" onclick="stopTrain()">STOP</button>
  </div>
</div>

<script>
let heartbeatTimer = null;
let pollTimer = null;
let running = false;
let spinMode = 0;
const PARAM_IDS = ['balls','perleg','fw','drive','traverse','pdrive','dirsettle'];
const BUTTON_IDS = ['startBtn','testDriveBtn'];

function startHeartbeat() {
  if (heartbeatTimer) return;
  heartbeatTimer = setInterval(() => fetch('/heartbeat'), 2000);
}
function stopHeartbeat() {
  clearInterval(heartbeatTimer);
  heartbeatTimer = null;
}

function setConfigEnabled(enabled) {
  PARAM_IDS.concat(BUTTON_IDS).forEach(id => { document.getElementById(id).disabled = !enabled; });
  document.querySelectorAll('.spin-opt').forEach(btn => { btn.disabled = !enabled; });
}

function setSpin(val, btn) {
  spinMode = val;
  document.querySelectorAll('.spin-opt').forEach(b => b.classList.remove('selected'));
  btn.classList.add('selected');
}

function beginRun(url) {
  running = true;
  setConfigEnabled(false);
  startHeartbeat();
  if (!pollTimer) pollTimer = setInterval(pollStatus, 300);
  return fetch(url);
}

function startTrain() {
  const params = PARAM_IDS.map(id => `${id}=${document.getElementById(id).value}`).join('&');
  beginRun(`/trainstart?${params}&spin=${spinMode}`);
}

function testDrive() {
  const drive = document.getElementById('drive').value;
  const traverse = document.getElementById('traverse').value;
  const perleg = document.getElementById('perleg').value;
  beginRun(`/testdrive?drive=${drive}&traverse=${traverse}&perleg=${perleg}`);
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
const ACTIVE_STATES = ['shoot', 'drive', 'testdrive'];
const STATUS_LABELS = { spinup: 'SPINNING UP', shoot: 'FIRING', postshoot: 'COOLDOWN', drive: 'DRIVING', postdrive: 'COOLDOWN', dirsettle: 'SETTLING', testdrive: 'TEST DRIVE' };

function pollStatus() {
  fetch('/trainstatus').then(r => r.json()).then(s => {
    const badge = document.getElementById('statusBadge');
    const isRunning = s.state !== 'idle';
    badge.className = 'status ' + (!isRunning ? 'idle' : (ACTIVE_STATES.includes(s.state) ? 'active' : 'pending'));
    badge.innerHTML = '<span class="dot"></span>' + (isRunning ? (STATUS_LABELS[s.state] || s.state.toUpperCase()) : 'IDLE');

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
document.getElementById('traverse').addEventListener('input', function() { document.getElementById('traverseVal').textContent = (this.value / 1000).toFixed(1) + 's'; });
document.getElementById('pdrive').addEventListener('input', function() { document.getElementById('pdriveVal').textContent = this.value + 'ms'; });
document.getElementById('dirsettle').addEventListener('input', function() { document.getElementById('dirsettleVal').textContent = this.value + 'ms'; });

pollTimer = setInterval(pollStatus, 300);
</script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html", PAGE_HTML);
}

void handleTrainStart() {
  if (server.hasArg("balls"))    totalBalls       = constrain(server.arg("balls").toInt(), 1, 100);
  if (server.hasArg("perleg"))   shotsPerLeg      = constrain(server.arg("perleg").toInt(), 1, 50);
  if (server.hasArg("fw"))       flywheelPct      = constrain(server.arg("fw").toInt(), 0, 100);
  if (server.hasArg("drive"))    drivePct         = constrain(server.arg("drive").toInt(), 0, 100);
  if (server.hasArg("traverse")) traverseMs       = constrain(server.arg("traverse").toInt(), 200, 15000);
  if (server.hasArg("pdrive"))   postDrivePauseMs = constrain(server.arg("pdrive").toInt(), 50, 5000);
  if (server.hasArg("dirsettle")) dirSettleMs     = constrain(server.arg("dirsettle").toInt(), 100, 3000);
  if (server.hasArg("spin"))     spinMode         = constrain(server.arg("spin").toInt(), -1, 1);

  shotIndex = 0;
  legShotCount = 0;
  direction = 1;
  driveEndsLeg = false;
  directionJustFlipped = false;
  armed = true;
  lastHeartbeat = millis();

  setServoAngle(CH_SERVO, GATE_CLOSED_ANGLE);
  setPulse(CH_DRIVE, DRIVE_NEUTRAL_US);
  setFlywheels();

  trainState = T_SPINUP;
  phaseStart = millis();
  Serial.println("Train run started");
  server.send(200, "text/plain", "started");
}

void handleTestDrive() {
  if (server.hasArg("drive"))    drivePct    = constrain(server.arg("drive").toInt(), 0, 100);
  if (server.hasArg("traverse")) traverseMs  = constrain(server.arg("traverse").toInt(), 200, 15000);
  if (server.hasArg("perleg"))   shotsPerLeg = constrain(server.arg("perleg").toInt(), 1, 50);

  direction = 1;  // test always drives forward - avoid showing a stale REVERSE badge left over from the last real run
  armed = true;
  lastHeartbeat = millis();

  testSegmentsRemaining = segmentsPerLeg();
  beginTestSegment();
  Serial.print("Traverse test started: "); Serial.print(testSegmentsRemaining); Serial.println(" segment(s)");
  server.send(200, "text/plain", "testing");
}

void handleTrainStop() {
  stopTrain();
  Serial.println("Train run stopped");
  server.send(200, "text/plain", "stopped");
}

void handleTrainStatus() {
  const char* stateStr;
  switch (trainState) {
    case T_IDLE:       stateStr = "idle";       break;
    case T_SPINUP:     stateStr = "spinup";     break;
    case T_SHOOT:      stateStr = "shoot";      break;
    case T_POST_SHOOT: stateStr = "postshoot";  break;
    case T_DRIVE:       stateStr = "drive";      break;
    case T_POST_DRIVE:  stateStr = "postdrive";  break;
    case T_DIR_SETTLE:  stateStr = "dirsettle";  break;
    case T_TEST_DRIVE:
    case T_TEST_PAUSE: stateStr = "testdrive";  break;
    default:           stateStr = "idle";       break;
  }
  // Test actions don't touch the ball sequence at all - report 0/0 rather
  // than whatever's left over from the last real run.
  bool isTest = (trainState == T_TEST_DRIVE || trainState == T_TEST_PAUSE);
  int ballDisplay = (trainState == T_IDLE || isTest) ? 0 : (shotIndex + 1);
  int totalDisplay = isTest ? 0 : totalBalls;
  String json = String("{\"state\":\"") + stateStr + "\",\"ball\":" + String(ballDisplay) +
                ",\"total\":" + String(totalDisplay) + ",\"direction\":" + String(direction) + "}";
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
  server.on("/testdrive", handleTestDrive);
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
