#include <limits.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>

#define LOGGER_INFO(message)     Serial.println(String("[INFO] ")    + message);
#define LOGGER_DEBUG(message)    Serial.println(String("[DEBUG] ")   + message);
#define LOGGER_WARNING(message)  Serial.println(String("[WARNING] ") + message);
#define LOGGER_ERROR(message)    Serial.println(String("[ERROR] ")   + message);

static const char* VERSION_NAME = "esp8266-cunning-roulette v0.10";

static const int A4988_RESET_PIN  = 16;
static const int A4988_SLEEP_PIN  = 0;
static const int A4988_STEP_PIN   = 4;
static const int A4988_DIR_PIN    = 5;
static const int A4988_MS1_PIN    = 12;
static const int A4988_MS2_PIN    = 13;
static const int A4988_MS3_PIN    = 14;
static const int BUTTON_PIN       = 2;
static const int LIMIT_SWITCH_PIN = 16;

static const unsigned int  MOTOR_STEPS           = 200U;
static const unsigned int  MICROSTEPS            = 16U;   // 1 or 2 or 4 or 8 or 16
static const unsigned long STEP                  = 1UL;
static const unsigned int  PULSE_INTERVAL_US     = 3U;
static const unsigned int  WAIT_TIME_PER_STEP_US = 156U;  // = (1000 * 1000 * 60) / (RPM * MOTOR_STEPS * MICROSTEPS);

struct RouletteArea {
  unsigned char start_step;
  unsigned char end_step;
};

// CAUTION: Follow the rules below
//          * Valid values ​​range from 0 to "MOTOR_STEPS".
//          * The array must be sorted in ascending order.
//          * Overlapping areas are not allowed.
//          * The array lengths of zero are not allowed.
static const RouletteArea AREAS_WHERE_THE_NEEDLE_CAN_STOP[] = {
  {   0,   3 }, //   0 degree ~   5 degree
  {  48,  53 }, //  85 degree ~  95 degree
  {  98, 103 }, // 175 degree ~ 185 degree
  { 148, 153 }, // 265 degree ~ 275 degree
  { 198, 200 }, // 355 degree ~ 359 degree
};

static unsigned int g_step = 0;

static void PowerOffStepperMotor(void);
static void PowerOnStepperMotor(void);
inline static void MoveStepperMotor(unsigned long steps);
static void GoToSleepMode(void);
ICACHE_RAM_ATTR static void CallbackAfterWakingUp(void);
inline static void SetStep(unsigned int step);
inline static unsigned int GetStep(void);
static void AddStep(unsigned int step);
static unsigned int FetchStepWhereTheNeedleCanStop(unsigned int step);

void setup()
{
  Serial.begin(9600);

  LOGGER_INFO(VERSION_NAME);

  pinMode(A4988_RESET_PIN, OUTPUT);
  pinMode(A4988_SLEEP_PIN, OUTPUT);
  pinMode(A4988_STEP_PIN, OUTPUT);
  pinMode(A4988_DIR_PIN, OUTPUT);
  pinMode(A4988_MS1_PIN, OUTPUT);
  pinMode(A4988_MS2_PIN, OUTPUT);
  pinMode(A4988_MS3_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LIMIT_SWITCH_PIN, INPUT);
}

void loop()
{
  LOGGER_DEBUG("Power off the stepper");
  PowerOffStepperMotor();

  LOGGER_DEBUG("Going to sleep...");
  GoToSleepMode();
  LOGGER_DEBUG("Wake up!!");

  LOGGER_DEBUG("Power on the stepper");
  PowerOnStepperMotor();

  LOGGER_DEBUG("Waiting for the button to be released...");
  while (digitalRead(BUTTON_PIN) == LOW) { }
  LOGGER_INFO("The button was released");

  LOGGER_INFO("Start turning the needle");
  LOGGER_DEBUG("Looking for the position where the needle points at 0 steps...");
  while (digitalRead(LIMIT_SWITCH_PIN) == LOW) {
    MoveStepperMotor(1);
  }
  SetStep(0);

  LOGGER_DEBUG("Waiting for the button to be pushed...");
  while (digitalRead(BUTTON_PIN) == HIGH) {
    MoveStepperMotor(STEP);
    AddStep(STEP);
  }

  // ----------------------------- CAUTION!! ----------------------------- //
  // Do not write output log here!!
  //
  // If you do it here, the needle stop process may not work properly.
  // This is because the output log process may take some time.
  // ----------------------------------------------------------------------//

  unsigned int distance_to_target = FetchStepWhereTheNeedleCanStop(GetStep());
  MoveStepperMotor(distance_to_target);
  AddStep(distance_to_target);

  LOGGER_INFO("The button was pushed");
  LOGGER_DEBUG(String("Stop the needle at ") + String(GetStep()) + String(" step."));

  // Eliminate chattering of pressed button
  delay(3); 

  LOGGER_DEBUG("Waiting for the button to be released...");
  while (digitalRead(BUTTON_PIN) == LOW) { }
  LOGGER_DEBUG("The button was released");
}

static void PowerOffStepperMotor(void)
{
  digitalWrite(A4988_RESET_PIN, LOW);
  digitalWrite(A4988_SLEEP_PIN, LOW);
  digitalWrite(A4988_DIR_PIN, LOW);
  digitalWrite(A4988_MS1_PIN, LOW);
  digitalWrite(A4988_MS2_PIN, LOW);
  digitalWrite(A4988_MS3_PIN, LOW);
}

static void PowerOnStepperMotor(void)
{
  digitalWrite(A4988_RESET_PIN, HIGH);
  digitalWrite(A4988_SLEEP_PIN, HIGH);
  digitalWrite(A4988_DIR_PIN, HIGH);

  const uint8_t ms_table[] = {0b000, 0b001, 0b010, 0b011, 0b111};
  for (int i = 0; i < sizeof(ms_table); i++) {
    if (MICROSTEPS & (1 << i)) {
      uint8_t mask = ms_table[i];
      digitalWrite(A4988_MS3_PIN, mask & 4);
      digitalWrite(A4988_MS2_PIN, mask & 2);
      digitalWrite(A4988_MS1_PIN, mask & 1);
      break;
    }
  }
}

inline static void MoveStepperMotor(unsigned long steps)
{
  for (int step = 0; step < steps; step++) {
    for (int microstep = 0; microstep < MICROSTEPS; microstep++) {
      digitalWrite(A4988_STEP_PIN, HIGH);
      delayMicroseconds(PULSE_INTERVAL_US);
      digitalWrite(A4988_STEP_PIN, LOW);
      delayMicroseconds(PULSE_INTERVAL_US);
      delayMicroseconds(WAIT_TIME_PER_STEP_US);
    }
  }
}

static void GoToSleepMode(void)
{
  WiFi.mode(WIFI_OFF);
  wifi_set_opmode_current(NULL_MODE); 
  wifi_fpm_set_sleep_type(LIGHT_SLEEP_T); 
  wifi_fpm_open();
  gpio_pin_wakeup_enable(BUTTON_PIN, GPIO_PIN_INTR_HILEVEL); 
  wifi_fpm_set_wakeup_cb(CallbackAfterWakingUp);
  wifi_fpm_do_sleep(0xFFFFFFF);
  delay(100);
}

ICACHE_RAM_ATTR static void CallbackAfterWakingUp(void)
{
  gpio_pin_wakeup_disable();
  wifi_fpm_close();
}

inline static void SetStep(unsigned int step)
{
  if (MOTOR_STEPS <= step) {
    LOGGER_ERROR(String("Invalid value!! (step = ") + String(step) + String (")"));
    return;
  }

  g_step = step;
}

inline static unsigned int GetStep(void)
{
  return g_step;
}

static void AddStep(unsigned int step)
{
  g_step += step;
  g_step = g_step % MOTOR_STEPS;
}

// TODO: Implement the feature that the needle can stop at the valid area randomly
static unsigned int FetchStepWhereTheNeedleCanStop(unsigned int step)
{
  unsigned int ret = -1;

  if (MOTOR_STEPS <= step) {
    LOGGER_ERROR(String("Invalid value!! (step = ") + String(step) + String (")"));
    return ret;
  }

  // The range is from 0 to LONG_MAX
  randomSeed(millis() % LONG_MAX);

  const int length = sizeof(AREAS_WHERE_THE_NEEDLE_CAN_STOP) / sizeof(AREAS_WHERE_THE_NEEDLE_CAN_STOP[0]);
  for (int i = 0; i < length; i++) {
    RouletteArea area = AREAS_WHERE_THE_NEEDLE_CAN_STOP[i];

    // This area will be the next nearest area
    if (step < area.start_step) {
      ret = random(area.start_step, area.end_step + 1) - step;
      break;
    }
    // This position of the needle now is in the valid area
    else if (area.start_step <= step && step <= area.end_step) {
      ret = 0;
      break;
    }
    else if (area.end_step < step) {
      // First area (AREAS_WHERE_THE_NEEDLE_CAN_STOP[0]) will be the nearest area
      if (i == length - 1) {
        ret = MOTOR_STEPS
          + random(AREAS_WHERE_THE_NEEDLE_CAN_STOP[0].start_step, AREAS_WHERE_THE_NEEDLE_CAN_STOP[0].end_step)
          - step;
        break;
      }
    } else {
      // Never comes
      LOGGER_ERROR("Fatal error!!");
    }
  }

  return ret;
}

