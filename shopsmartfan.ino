#include <TMCStepper.h>

#define EN_PIN     13
#define STEP_PIN   33
#define DIR_PIN    32
#define UART_TX    26    // ESP32 TX -> 1kΩ -> driver UART pin
#define UART_RX    27    // ESP32 RX taps the same bus
#define R_SENSE    0.11f // FYSETC/Dorhea TMC2208 V1.2 sense resistor

HardwareSerial DriverSerial(2);
TMC2208Stepper driver(&DriverSerial, R_SENSE);

void setup() {
  Serial.begin(115200);
  pinMode(EN_PIN, OUTPUT);
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  digitalWrite(EN_PIN, HIGH);                   // disable while configuring

  DriverSerial.begin(115200, SERIAL_8N1, UART_RX, UART_TX);
  driver.begin();

  driver.toff(5);                               // enable driver internals
  driver.rms_current(800);                      // mA — start conservative
  driver.microsteps(16);
  driver.intpol(true);                          // interpolate to 256
  driver.pwm_autoscale(true);                   // StealthChop auto-tune

  Serial.printf("Driver ver: %u\n", driver.version());

  digitalWrite(EN_PIN, LOW);                    // enable
}

void loop() {
  digitalWrite(DIR_PIN, HIGH);
  for (int i = 0; i < 3200; i++) {              // 1 revolution at 1/16 µstep
    digitalWrite(STEP_PIN, HIGH);
    delayMicroseconds(200);
    digitalWrite(STEP_PIN, LOW);
    delayMicroseconds(200);
  }
  delay(500);

  digitalWrite(DIR_PIN, LOW);
  for (int i = 0; i < 3200; i++) {
    digitalWrite(STEP_PIN, HIGH);
    delayMicroseconds(200);
    digitalWrite(STEP_PIN, LOW);
    delayMicroseconds(200);
  }
  delay(500);
}
