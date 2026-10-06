// =====================================================
// Smart Blind Walking Stick — Arduino Nano sensor node
// =====================================================
// Handles:
//   - HC-SR04 ultrasonic distance sensor
//   - Water / wet-ground sensor
//   - Vibration motor
//
// Water haptic pattern:
//   SHORT -> SHORT -> LONG -> PAUSE -> repeat
//
// The water pattern is NON-BLOCKING, so the Nano can
// continue reading the ultrasonic sensor and serial link
// while the vibration alert is running.
// =====================================================

#include <SoftwareSerial.h>

// ---------------------------------------------------
// Pin assignments
// ---------------------------------------------------

const int TRIG_PIN = 9;
const int ECHO_PIN = 10;
const int WATER_PIN = A2;
const int VIBRATION_PIN = 6; // PWM-capable pin, via transistor driver

// Nano D2 (RX) <- ESP32 GPIO12 (TX)
// Nano D3 (TX) -> [level shifter] -> ESP32 GPIO4 (RX)
SoftwareSerial espSerial(2, 3);

// ---------------------------------------------------
// Thresholds
// ---------------------------------------------------

const int DANGER_DISTANCE_CM = 30;
const int WARN_DISTANCE_CM = 100;

const int WATER_THRESHOLD = 500;

// ---------------------------------------------------
// Timing
// ---------------------------------------------------

const unsigned long reportInterval = 300;
const int significantChangeCm = 15;

// ---------------------------------------------------
// Water vibration pattern
// ---------------------------------------------------
// Pattern:
//   120 ms ON
//   100 ms OFF
//   120 ms ON
//   100 ms OFF
//   350 ms ON
//   900 ms OFF
//
// This gives a distinctive:
//   short - short - LONG
// pattern.
// ---------------------------------------------------

const unsigned long waterPatternTimes[] = {
  120,  // Step 0: short ON
  100,  // Step 1: OFF
  120,  // Step 2: short ON
  100,  // Step 3: OFF
  350,  // Step 4: long ON
  900   // Step 5: long pause
};

const bool waterPatternMotorState[] = {
  true,
  false,
  true,
  false,
  true,
  false
};

const int WATER_VIBRATION_INTENSITY = 255;

const int WATER_PATTERN_STEPS = 6;

int waterPatternStep = 0;
unsigned long waterStepStart = 0;
bool waterPatternRunning = false;

// ---------------------------------------------------
// Reporting state
// ---------------------------------------------------

unsigned long lastReportTime = 0;

int lastReportedDistance = -1;

bool lastWaterState = false;

// ---------------------------------------------------
// Setup
// ---------------------------------------------------

void setup()
{
  Serial.begin(9600);
  espSerial.begin(9600);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(VIBRATION_PIN, OUTPUT);
  pinMode(WATER_PIN, INPUT);

  digitalWrite(TRIG_PIN, LOW);
  analogWrite(VIBRATION_PIN, 0);

  Serial.println("Smart Walking Stick — Nano sensor node ready");
}

// =====================================================
// Read ultrasonic distance
// =====================================================

long readDistanceCM()
{
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);

  digitalWrite(TRIG_PIN, LOW);

  // 30ms timeout ≈ 5m maximum range
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);

  if (duration == 0)
  {
    return -1;
  }

  return duration * 0.0343 / 2;
}

// =====================================================
// Read water sensor
// =====================================================

bool readWaterDetected()
{
  int reading = analogRead(WATER_PIN);

  return reading > WATER_THRESHOLD;
}

// =====================================================
// Set vibration motor intensity
// =====================================================

void setVibration(int intensity)
{
  analogWrite(
    VIBRATION_PIN,
    constrain(intensity, 0, 255)
  );
}

// =====================================================
// Calculate obstacle vibration intensity
// =====================================================

int getObstacleIntensity(long distance)
{
  if (distance > 0 && distance < WARN_DISTANCE_CM)
  {
    // Very close obstacle
    if (distance < DANGER_DISTANCE_CM)
    {
      return 255;
    }

    // Gradually increase vibration as obstacle gets closer
    int intensity = map(
      distance,
      DANGER_DISTANCE_CM,
      WARN_DISTANCE_CM,
      255,
      60
    );

    return constrain(intensity, 60, 255);
  }

  return 0;
}

// =====================================================
// Start water vibration pattern
// =====================================================

void startWaterPattern(unsigned long now)
{
  waterPatternStep = 0;
  waterStepStart = now;
  waterPatternRunning = true;
}

// =====================================================
// Stop water vibration pattern
// =====================================================

void stopWaterPattern()
{
  waterPatternStep = 0;
  waterPatternRunning = false;
}

// =====================================================
// Update water vibration pattern
// =====================================================
// This function does NOT use delay().
// Therefore the Nano continues processing sensors.
// =====================================================

bool updateWaterPattern(bool waterDetected, unsigned long now)
{
  if (!waterDetected)
  {
    stopWaterPattern();
    return false;
  }

  // Start pattern immediately when water first appears
  if (!waterPatternRunning)
  {
    startWaterPattern(now);
  }

  // Advance through pattern steps when their time expires
  if (now - waterStepStart >= waterPatternTimes[waterPatternStep])
  {
    waterStepStart += waterPatternTimes[waterPatternStep];

    waterPatternStep++;

    if (waterPatternStep >= WATER_PATTERN_STEPS)
    {
      waterPatternStep = 0;
    }
  }

  return waterPatternMotorState[waterPatternStep];
}

// =====================================================
// Main loop
// =====================================================

void loop()
{
  unsigned long now = millis();

  // ---------------------------------------------------
  // Read sensors
  // ---------------------------------------------------

  long distance = readDistanceCM();

  bool waterDetected = readWaterDetected();

  // ---------------------------------------------------
  // Calculate obstacle vibration
  // ---------------------------------------------------

  int obstacleIntensity = getObstacleIntensity(distance);

  // ---------------------------------------------------
  // Update water vibration pattern
  // ---------------------------------------------------

  bool waterMotorOn =
      updateWaterPattern(waterDetected, now);

  // ---------------------------------------------------
  // Haptic priority logic
  // ---------------------------------------------------
  //
  // 1. VERY CLOSE OBSTACLE:
  //      continuous maximum vibration
  //
  // 2. WATER:
  //      short-short-long pattern
  //
  // 3. NORMAL OBSTACLE:
  //      distance-based vibration
  //
  // 4. NOTHING:
  //      motor OFF
  // ---------------------------------------------------

  if (distance > 0 && distance < DANGER_DISTANCE_CM)
  {
    // Highest priority:
    // dangerous obstacle is very close.
    setVibration(255);
  }
  else if (waterDetected)
  {
    if (waterMotorOn)
    {
      // Water pulse.
      //
      // Use maximum of water and obstacle intensity
      // so an obstacle warning is not completely lost.
      setVibration(
        max(WATER_VIBRATION_INTENSITY, obstacleIntensity)
      );
    }
    else
    {
      // During water pattern pause, allow normal
      // obstacle vibration to continue.
      setVibration(obstacleIntensity);
    }
  }
  else
  {
    // Normal obstacle feedback
    setVibration(obstacleIntensity);
  }

  // ---------------------------------------------------
  // Report events to ESP32
  // ---------------------------------------------------

  if (now - lastReportTime >= reportInterval)
  {
    // -----------------------------------------------
    // Water detected
    // -----------------------------------------------

    if (waterDetected && !lastWaterState)
    {
      espSerial.println("WATER");
      Serial.println("-> WATER");
    }

    // -----------------------------------------------
    // Check significant distance change
    // -----------------------------------------------

    bool distanceChangedSignificantly =
        (lastReportedDistance == -1) ||
        (distance <= 0) ||
        (abs((int)distance - lastReportedDistance) >
         significantChangeCm);

    // -----------------------------------------------
    // Obstacle detected
    // -----------------------------------------------

    if (distance > 0 &&
        distance < WARN_DISTANCE_CM &&
        distanceChangedSignificantly)
    {
      espSerial.print("OBSTACLE:");
      espSerial.println(distance);

      Serial.print("-> OBSTACLE:");
      Serial.println(distance);

      lastReportedDistance = distance;
    }

    // -----------------------------------------------
    // Obstacle cleared
    // -----------------------------------------------

    else if ((distance <= 0 ||
              distance >= WARN_DISTANCE_CM) &&
             lastReportedDistance != -1)
    {
      espSerial.println("CLEAR");

      Serial.println("-> CLEAR");

      lastReportedDistance = -1;
    }

    // Save water state
    lastWaterState = waterDetected;

    lastReportTime = now;
  }

  // ---------------------------------------------------
  // Small loop delay
  // ---------------------------------------------------

  delay(50);
}
