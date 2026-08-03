#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver(0x40);

const int CH_RIGHT = 0;  // match your current channel assignment
const int CH_LEFT  = 2;

const int MIN_US = 1000;
const int MAX_US = 2000;

void waitForEnter() {
  while (Serial.available()) Serial.read();       // clear any leftover input
  while (!Serial.available()) delay(50);          // wait for new input
  while (Serial.available()) Serial.read();       // clear it out again
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(21, 22);
  pwm.begin();
  pwm.setPWMFreq(50);

  Serial.println("=== ESC CALIBRATION ===");
  Serial.println("Make sure the ESC battery switch is OFF right now.");
  Serial.println("Type anything in the Serial Monitor box above and hit Enter when ready.");
  waitForEnter();

  Serial.println();
  Serial.println("Sending MAX throttle signal now.");
  Serial.println("Motor should NOT spin during this step - that's correct.");
  pwm.writeMicroseconds(CH_RIGHT, MAX_US);
  pwm.writeMicroseconds(CH_LEFT, MAX_US);

  Serial.println();
  Serial.println(">>> Now flip the ESC battery switch ON. <<<");
  Serial.println("Listen for the ESC's max-throttle confirmation tone.");
  Serial.println("Once you hear it, type anything and hit Enter to continue.");
  waitForEnter();

  Serial.println();
  Serial.println("Dropping to MIN throttle now. Motor should stay off.");
  pwm.writeMicroseconds(CH_RIGHT, MIN_US);
  pwm.writeMicroseconds(CH_LEFT, MIN_US);

  Serial.println("Listen for a second, DIFFERENT tone confirming min throttle.");
  Serial.println("That means calibration is saved.");
  Serial.println();
  Serial.println("Now power cycle the battery (switch off, wait a few seconds, switch on).");
  Serial.println("You should hear the NORMAL arm tone this time, not a calibration tone -");
  Serial.println("that confirms it actually stuck.");
}

void loop() {
  // Hold at minimum throttle - nothing else to do here.
  pwm.writeMicroseconds(CH_RIGHT, MIN_US);
  pwm.writeMicroseconds(CH_LEFT, MIN_US);
}
