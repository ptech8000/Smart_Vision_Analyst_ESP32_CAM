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
// while the vibration alert is active.

// [Uploaded source file preserved verbatim.]

