#define BLYNK_TEMPLATE_ID   "TMPLxxxxxxxx"
#define BLYNK_TEMPLATE_NAME "Home"
#define BLYNK_AUTH_TOKEN    "YOUR_NEW_BLYNK_AUTH_TOKEN"clear

#include <ESP8266WiFi.h>
#include <BlynkSimpleEsp8266.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <math.h>
#include <time.h>

// Wi-Fi credentials
char ssid[] = "OnMyLevel";
char pass[] = "          ";

// MAX7219 8x32 matrix wiring
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 4

#define DATA_PIN D7
#define CS_PIN   D8
#define CLK_PIN  D5

MD_Parola display(
  HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES
);

MD_MAX72XX* matrix = nullptr;

// --------------------------------------------------
// DISPLAY SETTINGS
// --------------------------------------------------

// Start blank. A message appears when received from V0.
char message[201] = "";

uint16_t scrollSpeed = 70;
uint8_t brightness = 5;

// V4 text effect:
// 0 Left, 1 Right, 2 Up, 3 Down,
// 4 Random Direction, 5 Random Effect
uint8_t textMode = 0;

// V5 pattern:
// 0 Random, 1 Bouncing Dot, 2 Falling Pixels,
// 3 Wave, 4 Checkerboard, 5 Heartbeat,
// 6 Stick Men (fighting / dancing), 7 Sparkle
uint8_t idleMode = 0;
uint8_t activePattern = 1;

// Patterns run only when explicitly selected.
bool patternsRequested = false;

// V6 clock/date mode
bool clockModeEnabled = false;
bool showingDate = false;
uint32_t clockPhaseStarted = 0;
int lastClockSecond = -1;
char clockText[48] = "";
const uint32_t CLOCK_TIME_MS = 40000;
const uint32_t CLOCK_DATE_MS = 7500;

// --------------------------------------------------
// ALARM (V7 time, V8 on/off, V9 stop)
// Passive buzzer (for melody): + to D1 (GPIO5), - to GND.
// --------------------------------------------------

#define BUZZER_PIN D1
#define BUZZER_ON  HIGH   // use LOW/HIGH swapped for a low-level-trigger buzzer module
#define BUZZER_OFF LOW

bool alarmEnabled = false;
bool alarmTimeSet = false;
uint8_t alarmHour = 0;
uint8_t alarmMinute = 0;
int lastAlarmKey = -1;            // prevents re-triggering in the same minute

bool alarmRinging = false;
bool wasClockMode = false;
uint32_t alarmStarted = 0;

const uint32_t ALARM_MAX_MS = 60000;   // auto-stop after 60 seconds

// 1 = passive buzzer / speaker: plays real notes with tone().
// 0 = active buzzer: cannot change pitch, so only the melody's rhythm plays.
#define PASSIVE_BUZZER 1

// Melody: frequency in Hz (0 = rest) and duration in ms. Loops until stopped.
struct Note {
  uint16_t freq;
  uint16_t ms;
};

const Note alarmMelody[] = {
  {523, 150}, {659, 150}, {784, 150}, {1047, 300},
  {0, 100},
  {784, 150}, {1047, 150}, {1319, 400},
  {0, 150},
  {1047, 150}, {1319, 150}, {1568, 500},
  {0, 700}
};
const uint8_t MELODY_LEN = sizeof(alarmMelody) / sizeof(alarmMelody[0]);

uint8_t melodyIndex = 0;
uint32_t noteStarted = 0;
bool noteLoaded = false;

void silenceBuzzer() {
#if PASSIVE_BUZZER
  noTone(BUZZER_PIN);
#endif
  digitalWrite(BUZZER_PIN, BUZZER_OFF);
}

void playNote(uint16_t freq) {
#if PASSIVE_BUZZER
  if (freq > 0) {
    tone(BUZZER_PIN, freq);
  } else {
    silenceBuzzer();
  }
#else
  digitalWrite(BUZZER_PIN, freq > 0 ? BUZZER_ON : BUZZER_OFF);
#endif
}

// Last message sent from V0; shown while the alarm rings.
char alarmMessage[201] = "";

void stopAlarm();   // forward declaration, used by the V3 clear button

// Animation timing
uint32_t lastFrame = 0;
uint32_t lastPatternChange = 0;
uint32_t frameNumber = 0;

// --------------------------------------------------
// RANDOM BOUNCING DOT
// Entire matrix: X = 0-31, Y = 0-7
// --------------------------------------------------

int dotX = 16;
int dotY = 4;
int dotDX = 1;
int dotDY = 1;
uint8_t bounceCount = 0;

// --------------------------------------------------
// STICK MEN (pattern 6)
// Two stick men that randomly switch between
// fighting and dancing.
// --------------------------------------------------

// Fight poses
#define FIGHT_GUARD  0
#define FIGHT_PUNCH  1
#define FIGHT_KICK   2
#define FIGHT_RECOIL 3

int manX[2] = {9, 22};          // body centre column of each man
int manFace[2] = {1, -1};       // +1 faces right, -1 faces left
uint8_t manPose[2] = {0, 0};
bool stickFight = true;         // true = fighting, false = dancing
uint16_t stickModeFrames = 0;   // frames left before mode switch
uint8_t stickPoseFrames = 0;    // frames left before next pose change

void initStickMen() {
  manX[0] = 9;
  manX[1] = 22;
  manFace[0] = 1;
  manFace[1] = -1;
  manPose[0] = 0;
  manPose[1] = 0;
  stickFight = random(0, 2);
  stickModeFrames = random(30, 60);   // 3-6 seconds at 10 fps
  stickPoseFrames = 0;
}

// Safe pixel plot (row = y, column = x)
void px(int x, int y) {
  if (matrix == nullptr) return;
  if (x < 0 || x > 31 || y < 0 || y > 7) return;
  matrix->setPoint(y, x, true);
}

// Draw one stick man.
// cx = body column, f = facing (+1 right / -1 left)
void drawStickMan(int cx, int f, bool fighting, uint8_t pose, uint32_t beat) {
  // Head and torso
  px(cx, 0);
  for (uint8_t y = 1; y <= 4; y++) px(cx, y);

  // Default legs (small inverted V)
  bool defaultLegs = true;

  if (fighting) {
    switch (pose) {
      case FIGHT_GUARD:
        // Fists up in front
        px(cx + f, 2);
        px(cx + 2 * f, 1);
        px(cx - f, 2);
        break;

      case FIGHT_PUNCH:
        // Front arm fully extended, back arm tucked
        px(cx + f, 2);
        px(cx + 2 * f, 2);
        px(cx + 3 * f, 2);
        px(cx - f, 3);
        break;

      case FIGHT_KICK:
        // Standing leg + extended kicking leg
        px(cx - f, 2);
        px(cx + f, 2);
        px(cx - f, 5);
        px(cx - f, 6);
        px(cx - f, 7);
        px(cx, 5);
        px(cx + f, 5);
        px(cx + 2 * f, 5);
        px(cx + 3 * f, 5);
        defaultLegs = false;
        break;

      case FIGHT_RECOIL:
        // Knocked back: head leans away, arms flailing
        px(cx - f, 0);
        px(cx, 0);
        px(cx - f, 1);
        px(cx + 1, 1);
        px(cx - 2, 1);
        px(cx + 2, 2);
        px(cx - 2, 2);
        break;
    }
  } else {
    // Dance poses
    switch (pose) {
      case 0:
        // Both arms up (celebrate)
        px(cx - 1, 1);
        px(cx - 2, 0);
        px(cx + 1, 1);
        px(cx + 2, 0);
        break;

      case 1:
        // Arms straight out
        px(cx - 1, 2);
        px(cx - 2, 2);
        px(cx + 1, 2);
        px(cx + 2, 2);
        break;

      case 2:
        // Disco: one arm up, one arm down
        px(cx - 1, 1);
        px(cx - 2, 0);
        px(cx + 1, 3);
        px(cx + 2, 4);
        break;

      case 3:
        // Disco, other side
        px(cx + 1, 1);
        px(cx + 2, 0);
        px(cx - 1, 3);
        px(cx - 2, 4);
        break;
    }

    // Dancing legs alternate between together and wide apart
    if (beat % 2 == 0) {
      px(cx - 1, 5);
      px(cx - 2, 6);
      px(cx - 3, 7);
      px(cx + 1, 5);
      px(cx + 2, 6);
      px(cx + 3, 7);
      defaultLegs = false;
    }
  }

  if (defaultLegs) {
    px(cx, 5);
    px(cx - 1, 6);
    px(cx - 1, 7);
    px(cx + 1, 6);
    px(cx + 1, 7);
  }
}

// Advance the stick-man animation by one frame.
void updateStickMen() {
  // Randomly switch between fighting and dancing
  if (stickModeFrames == 0) {
    stickFight = !stickFight;
    stickModeFrames = random(30, 70);
    stickPoseFrames = 0;
    manPose[0] = 0;
    manPose[1] = 0;
  } else {
    stickModeFrames--;
  }

  // Only change poses / positions every few frames
  if (stickPoseFrames > 0) {
    stickPoseFrames--;
    return;
  }
  stickPoseFrames = random(2, 5);

  if (stickFight) {
    int gap = manX[1] - manX[0];

    // Keep them facing each other
    manFace[0] = 1;
    manFace[1] = -1;

    // Close the distance if too far apart
    if (gap > 7) {
      manX[0] += 1;
      manX[1] -= 1;
      manPose[0] = FIGHT_GUARD;
      manPose[1] = FIGHT_GUARD;
      return;
    }

    // Both pick an action. Recoil poses are set by hits below.
    for (uint8_t i = 0; i < 2; i++) {
      uint8_t r = random(0, 10);
      if (r < 3)      manPose[i] = FIGHT_GUARD;
      else if (r < 7) manPose[i] = FIGHT_PUNCH;
      else            manPose[i] = FIGHT_KICK;
    }

    // Work out hits (punch reach 3, kick reach 3, plus body width)
    bool hit0 = (manPose[0] == FIGHT_PUNCH || manPose[0] == FIGHT_KICK) && gap <= 4;
    bool hit1 = (manPose[1] == FIGHT_PUNCH || manPose[1] == FIGHT_KICK) && gap <= 4;

    if (hit0 && !hit1) {
      manPose[1] = FIGHT_RECOIL;
      manX[1] += 1;                       // pushed back
    } else if (hit1 && !hit0) {
      manPose[0] = FIGHT_RECOIL;
      manX[0] -= 1;
    } else if (hit0 && hit1) {
      // Trade blows, both stumble back
      manX[0] -= 1;
      manX[1] += 1;
    } else if (gap > 5 && random(0, 3) == 0) {
      // Step in
      manX[0] += 1;
      manX[1] -= 1;
    } else if (gap <= 3) {
      // Too close with no hit, back off a little
      manX[0] -= 1;
      manX[1] += 1;
    }

    manX[0] = constrain(manX[0], 3, 27);
    manX[1] = constrain(manX[1], 4, 28);
    if (manX[1] - manX[0] < 3) {
      manX[0] = constrain(manX[1] - 3, 3, 27);
    }
  } else {
    // Dancing: each man picks a random dance move and shuffles a little
    for (uint8_t i = 0; i < 2; i++) {
      manPose[i] = random(0, 4);
      manFace[i] = random(0, 2) ? 1 : -1;

      int shuffle = random(-1, 2);   // -1, 0, +1
      manX[i] += shuffle;
    }
    manX[0] = constrain(manX[0], 4, 14);
    manX[1] = constrain(manX[1], 17, 27);
  }
}

void drawStickMen() {
  drawStickMan(manX[0], manFace[0], stickFight, manPose[0], frameNumber / 3);
  drawStickMan(manX[1], manFace[1], stickFight, manPose[1], frameNumber / 3);
}

// --------------------------------------------------
// TEXT DISPLAY
// --------------------------------------------------

void startMessage() {
  display.displayClear();
  patternsRequested = false;

  if (message[0] == '\0') {
    return;
  }

  textEffect_t effectIn = PA_SCROLL_LEFT;
  textEffect_t effectOut = PA_SCROLL_LEFT;

  switch (textMode) {
    case 0:
      effectIn = PA_SCROLL_LEFT;
      effectOut = PA_SCROLL_LEFT;
      break;

    case 1:
      effectIn = PA_SCROLL_RIGHT;
      effectOut = PA_SCROLL_RIGHT;
      break;

    case 2:
      effectIn = PA_SCROLL_UP;
      effectOut = PA_SCROLL_UP;
      break;

    case 3:
      effectIn = PA_SCROLL_DOWN;
      effectOut = PA_SCROLL_DOWN;
      break;

    case 4: {
      uint8_t direction = random(0, 4);

      if (direction == 0) {
        effectIn = PA_SCROLL_LEFT;
        effectOut = PA_SCROLL_LEFT;
      } else if (direction == 1) {
        effectIn = PA_SCROLL_RIGHT;
        effectOut = PA_SCROLL_RIGHT;
      } else if (direction == 2) {
        effectIn = PA_SCROLL_UP;
        effectOut = PA_SCROLL_UP;
      } else {
        effectIn = PA_SCROLL_DOWN;
        effectOut = PA_SCROLL_DOWN;
      }
      break;
    }

    case 5: {
      const textEffect_t effects[] = {
        PA_SCROLL_LEFT,
        PA_SCROLL_RIGHT,
        PA_FADE,
        PA_DISSOLVE,
        PA_BLINDS,
        PA_WIPE,
        PA_SCAN_HORIZ,
        PA_SCAN_VERT,
        PA_OPENING,
        PA_CLOSING
      };

      uint8_t pick = random(0, 10);
      effectIn = effects[pick];
      effectOut = effects[pick];
      break;
    }
  }

  display.displayText(
    message,
    PA_LEFT,
    scrollSpeed,
    0,
    effectIn,
    effectOut
  );

  display.displayReset();

  Serial.print("[MESSAGE] ");
  Serial.println(message);
}

// --------------------------------------------------
// CLOCK AND DATE DISPLAY (V6), using Kenya time (UTC+3)
// --------------------------------------------------

void renderClockTime() {
  time_t now = time(nullptr);
  struct tm timeInfo;
  if (now < 100000 || localtime_r(&now, &timeInfo) == nullptr) {
    snprintf(clockText, sizeof(clockText), "SYNCING TIME");
    display.displayClear();
    display.displayText(clockText, PA_LEFT, 35, 0,
                        PA_SCROLL_LEFT, PA_SCROLL_LEFT);
    display.displayReset();
    return;
  }

  // Blink the colon on alternate seconds, using the actual clock seconds.
  const char* separator = (timeInfo.tm_sec % 2 == 0) ? ":" : " ";
  snprintf(clockText, sizeof(clockText), "%02d%s%02d",
           timeInfo.tm_hour, separator, timeInfo.tm_min);
  display.displayClear();
  display.displayText(clockText, PA_CENTER, 0, 0, PA_PRINT, PA_NO_EFFECT);
  display.displayReset();
  lastClockSecond = timeInfo.tm_sec;
}

void startClockFace(bool dateFace) {
  if (!clockModeEnabled) return;
  showingDate = dateFace;
  display.displayClear();

  time_t now = time(nullptr);
  struct tm timeInfo;
  if (now < 100000 || localtime_r(&now, &timeInfo) == nullptr) {
    snprintf(clockText, sizeof(clockText), "SYNCING TIME");
    display.displayText(clockText, PA_LEFT, 35, 0,
                        PA_SCROLL_LEFT, PA_SCROLL_LEFT);
  } else if (dateFace) {
    // Full date is longer than the 8x32 display, so scroll it across.
    const char* weekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                              "Thursday", "Friday", "Saturday"};
    const char* months[] = {"January", "February", "March", "April", "May", "June",
                            "July", "August", "September", "October", "November", "December"};
    snprintf(clockText, sizeof(clockText), "%s, %s %d, %d",
             weekdays[timeInfo.tm_wday], months[timeInfo.tm_mon],
             timeInfo.tm_mday, timeInfo.tm_year + 1900);
    display.displayText(clockText, PA_LEFT, 25, 0,
                        PA_SCROLL_LEFT, PA_SCROLL_LEFT);
  } else {
    // Render the digital clock; updateClockDisplay blinks its colon each second.
    renderClockTime();
  }

  display.displayReset();
  clockPhaseStarted = millis();
  Serial.printf("[CLOCK] Showing %s: %s\n", dateFace ? "date" : "time", clockText);
}

void updateClockDisplay() {
  if (!clockModeEnabled) return;
  uint32_t now = millis();
  uint32_t duration = showingDate ? CLOCK_DATE_MS : CLOCK_TIME_MS;
  if (now - clockPhaseStarted >= duration) {
    startClockFace(!showingDate);
    return;
  }

  // Refresh only when the seconds value changes. Do not reset the 40-second phase.
  if (!showingDate) {
    time_t epoch = time(nullptr);
    struct tm timeInfo;
    if (epoch >= 100000 && localtime_r(&epoch, &timeInfo) != nullptr &&
        timeInfo.tm_sec != lastClockSecond) {
      renderClockTime();
    }
  }
  if (display.displayAnimate()) display.displayReset();
}

// --------------------------------------------------
// BLYNK CONNECTION
// --------------------------------------------------

BLYNK_CONNECTED() {
  Serial.println("[BLYNK] Connected.");

  // Restore settings only.
  // Do not automatically restart patterns after reconnecting.
  Blynk.syncVirtual(V1, V2, V4, V7, V10, V8);  // Clock/date starts automatically after every restart.
}

// --------------------------------------------------
// V0: MESSAGE INPUT
// --------------------------------------------------

BLYNK_WRITE(V0) {
  String incoming = param.asStr();
  incoming.trim();
  incoming.toCharArray(message, sizeof(message));

  // Remember the latest non-empty message for the alarm.
  if (message[0] != '\0') {
    strlcpy(alarmMessage, message, sizeof(alarmMessage));
  }

  if (message[0] == '\0') {
    patternsRequested = false;
    display.displayClear();
    Serial.println("[MESSAGE] Empty; display stays blank.");
  } else {
    startMessage();
  }
}

// --------------------------------------------------
// V1: SCROLL SPEED
// --------------------------------------------------

BLYNK_WRITE(V1) {
  scrollSpeed = constrain(param.asInt(), 20, 200);
  Serial.printf("[SPEED] %u\n", scrollSpeed);

  if (message[0] != '\0') {
    startMessage();
  }
}

// --------------------------------------------------
// V2: BRIGHTNESS
// --------------------------------------------------

BLYNK_WRITE(V2) {
  brightness = constrain(param.asInt(), 0, 15);
  display.setIntensity(brightness);
  Serial.printf("[BRIGHTNESS] %u\n", brightness);
}

// --------------------------------------------------
// V3: CLEAR DISPLAY
// --------------------------------------------------

BLYNK_WRITE(V3) {
  if (param.asInt() == 1) {
    // Stop the alarm first if it is ringing.
    if (alarmRinging) {
      stopAlarm();
    }

    // Clear the message, the saved alarm message and the alarm settings.
    message[0] = '\0';
    alarmMessage[0] = '\0';
    patternsRequested = false;

    alarmEnabled = false;
    alarmTimeSet = false;
    alarmHour = 0;
    alarmMinute = 0;
    lastAlarmKey = -1;

    display.displayClear();

    // Also reset the values Blynk Cloud has stored, so they are not
    // restored on the next reconnect or restart.
    Blynk.virtualWrite(V0, "");
    Blynk.virtualWrite(V7, "");
    Blynk.virtualWrite(V8, 0);
    Blynk.virtualWrite(V10, 0);

    Serial.println("[DISPLAY] Cleared everything: message, alarm message and alarm settings.");
  }
}

// --------------------------------------------------
// V4: TEXT EFFECT
// --------------------------------------------------

BLYNK_WRITE(V4) {
  textMode = constrain(param.asInt(), 0, 5);
  Serial.printf("[TEXT EFFECT] %u\n", textMode);

  if (message[0] != '\0') {
    startMessage();
  }
}

// --------------------------------------------------
// V6: Set a Blynk Switch widget to virtual pin V6 (0=off, 1=on).
// --------------------------------------------------

BLYNK_WRITE(V6) {
  clockModeEnabled = (param.asInt() == 1);
  display.displayClear();
  if (clockModeEnabled) {
    patternsRequested = false;
    startClockFace(false);  // Start with time, then date after 40 seconds.
    Serial.println("[CLOCK] Clock/date mode enabled.");
  } else {
    Serial.println("[CLOCK] Clock/date mode disabled.");
    if (message[0] != '\0') startMessage();
    else {
      patternsRequested = false;
      display.displayClear();
    }
  }
}

// --------------------------------------------------
// ALARM LOGIC
// --------------------------------------------------

void stopAlarm() {
  silenceBuzzer();
  alarmRinging = false;
  noteLoaded = false;
  display.displayClear();

  // Restore whatever was showing before the alarm.
  clockModeEnabled = wasClockMode;
  if (clockModeEnabled) {
    startClockFace(false);
  } else if (message[0] != '\0') {
    startMessage();
  } else {
    patternsRequested = false;
  }

  Serial.println("[ALARM] Stopped.");
}

void startAlarm() {
  alarmRinging = true;
  alarmStarted = millis();
  melodyIndex = 0;
  noteLoaded = false;

  wasClockMode = clockModeEnabled;
  clockModeEnabled = false;
  patternsRequested = false;

  // Show the saved message, or a default if none was ever sent.
  const char* text = (alarmMessage[0] != '\0') ? alarmMessage : "ALARM!";
  display.displayClear();
  display.displayText(text, PA_LEFT, scrollSpeed, 0,
                      PA_SCROLL_LEFT, PA_SCROLL_LEFT);
  display.displayReset();

  Serial.println("[ALARM] Ringing!");
}

void checkAlarm() {
  static uint32_t lastCheck = 0;
  uint32_t nowMs = millis();
  if (nowMs - lastCheck < 500) return;
  lastCheck = nowMs;

  if (!alarmEnabled || !alarmTimeSet || alarmRinging) return;

  time_t now = time(nullptr);
  struct tm timeInfo;
  if (now < 100000 || localtime_r(&now, &timeInfo) == nullptr) return;

  int key = timeInfo.tm_yday * 1440 + timeInfo.tm_hour * 60 + timeInfo.tm_min;

  if (timeInfo.tm_hour == alarmHour &&
      timeInfo.tm_min == alarmMinute &&
      key != lastAlarmKey) {
    lastAlarmKey = key;
    startAlarm();
  }
}

void updateAlarmRinging() {
  uint32_t now = millis();

  if (now - alarmStarted >= ALARM_MAX_MS) {
    stopAlarm();
    return;
  }

  // Non-blocking melody: move to the next note when the current one ends.
  if (!noteLoaded || now - noteStarted >= alarmMelody[melodyIndex].ms) {
    if (noteLoaded) {
      melodyIndex = (melodyIndex + 1) % MELODY_LEN;
    }
    noteLoaded = true;
    noteStarted = now;
    playNote(alarmMelody[melodyIndex].freq);
  }

  // Keep the message scrolling.
  if (display.displayAnimate()) {
    display.displayReset();
  }
}

// --------------------------------------------------
// V7: ALARM TIME
//  Option A: a Text Input widget on V7 where you type HH:MM (e.g. 07:30)
//  Option B: a Slider on V7 for the hour (0-23) plus a Slider on V10 for
//            the minute (0-59)
// --------------------------------------------------

BLYNK_WRITE(V7) {
  String s = param.asStr();
  s.trim();

  int colon = s.indexOf(':');

  if (colon > 0) {
    // Text format "HH:MM"
    int h = s.substring(0, colon).toInt();
    int m = s.substring(colon + 1).toInt();

    if (h >= 0 && h <= 23 && m >= 0 && m <= 59) {
      alarmHour = h;
      alarmMinute = m;
      alarmTimeSet = true;
      lastAlarmKey = -1;
      Serial.printf("[ALARM] Set for %02u:%02u\n", alarmHour, alarmMinute);
    } else {
      Serial.println("[ALARM] Invalid time. Use HH:MM, e.g. 07:30");
    }
  } else if (s.length() > 0) {
    // Slider: hour only
    alarmHour = constrain(s.toInt(), 0, 23);
    alarmTimeSet = true;
    lastAlarmKey = -1;
    Serial.printf("[ALARM] Hour %02u, minute %02u\n", alarmHour, alarmMinute);
  } else {
    alarmTimeSet = false;
    Serial.println("[ALARM] Time cleared.");
  }
}

// --------------------------------------------------
// V10: ALARM MINUTE (Slider 0-59, only needed for Option B)
// --------------------------------------------------

BLYNK_WRITE(V10) {
  alarmMinute = constrain(param.asInt(), 0, 59);
  alarmTimeSet = true;
  lastAlarmKey = -1;
  Serial.printf("[ALARM] Hour %02u, minute %02u\n", alarmHour, alarmMinute);
}

// --------------------------------------------------
// V8: ALARM ON/OFF (Switch widget)
// --------------------------------------------------

BLYNK_WRITE(V8) {
  alarmEnabled = (param.asInt() == 1);
  lastAlarmKey = -1;
  Serial.printf("[ALARM] %s\n", alarmEnabled ? "Enabled" : "Disabled");

  if (!alarmEnabled && alarmRinging) {
    stopAlarm();
  }
}

// --------------------------------------------------
// V9: STOP ALARM (Button widget, push)
// --------------------------------------------------

BLYNK_WRITE(V9) {
  if (param.asInt() == 1 && alarmRinging) {
    stopAlarm();
  }
}

// --------------------------------------------------
// V5: PATTERN SELECTOR
// --------------------------------------------------

BLYNK_WRITE(V5) {
  idleMode = constrain(param.asInt(), 0, 7);

  // Selecting a pattern explicitly starts animation.
  message[0] = '\0';
  patternsRequested = true;

  frameNumber = 0;
  lastFrame = 0;
  lastPatternChange = 0;
  bounceCount = 0;

  // Start the dot at a random position with a random direction.
  dotX = random(0, 32);
  dotY = random(0, 8);
  dotDX = random(0, 2) ? 1 : -1;
  dotDY = random(0, 2) ? 1 : -1;

  // Reset the stick men.
  initStickMen();

  if (idleMode == 0) {
    activePattern = random(1, 8);
  } else {
    activePattern = idleMode;
  }

  display.displayClear();

  Serial.printf("[PATTERN REQUESTED] %u\n", idleMode);
}

// --------------------------------------------------
// DRAW SELECTED IDLE PATTERN
// --------------------------------------------------

void drawIdlePattern(uint8_t pattern) {
  if (matrix == nullptr) {
    return;
  }

  matrix->clear();

  switch (pattern) {

    case 1: {
      // Random bouncing dot across all 32x8 pixels.
      matrix->setPoint(dotY, dotX, true);
      break;
    }

    case 2: {
      // Random falling-pixel effect.
      for (uint8_t i = 0; i < 14; i++) {
        uint8_t x = random(0, 32);
        uint8_t y = random(0, 8);
        matrix->setPoint(y, x, true);
      }
      break;
    }

    case 3: {
      // Moving wave.
      for (uint8_t x = 0; x < 32; x++) {
        float angle = (x + frameNumber) * 0.30;
        uint8_t y = (uint8_t)(3.5 + 3.0 * sin(angle));

        matrix->setPoint(y, x, true);

        if (y > 0) {
          matrix->setPoint(y - 1, x, true);
        }
      }
      break;
    }

    case 4: {
      // Animated checkerboard.
      for (uint8_t y = 0; y < 8; y++) {
        for (uint8_t x = 0; x < 32; x++) {
          if (((x + y + frameNumber / 5) % 2) == 0) {
            matrix->setPoint(y, x, true);
          }
        }
      }
      break;
    }

    case 5: {
      // Two pulsing hearts.
      const uint8_t heart[8] = {
        0b00000000,
        0b01100110,
        0b11111111,
        0b11111111,
        0b01111110,
        0b00111100,
        0b00011000,
        0b00000000
      };

      bool visible = (frameNumber % 12) < 8;

      if (visible) {
        for (uint8_t y = 0; y < 8; y++) {
          for (uint8_t x = 0; x < 8; x++) {
            if (heart[y] & (1 << (7 - x))) {
              matrix->setPoint(y, x + 8, true);
              matrix->setPoint(y, x + 16, true);
            }
          }
        }
      }
      break;
    }

    case 6: {
      // Two stick men randomly fighting and dancing.
      drawStickMen();
      break;
    }

    case 7: {
      // Random sparkle.
      for (uint8_t i = 0; i < 25; i++) {
        matrix->setPoint(
          random(0, 8),
          random(0, 32),
          true
        );
      }
      break;
    }
  }
}

// --------------------------------------------------
// UPDATE BOUNCING DOT
// --------------------------------------------------

void updateBouncingDot() {
  dotX += dotDX;
  dotY += dotDY;

  // Bounce at the left and right edges.
  if (dotX <= 0 || dotX >= 31) {
    dotX = constrain(dotX, 0, 31);
    dotDX = -dotDX;
    bounceCount++;
  }

  // Bounce at the top and bottom edges.
  if (dotY <= 0 || dotY >= 7) {
    dotY = constrain(dotY, 0, 7);
    dotDY = -dotDY;
    bounceCount++;
  }

  // Occasionally choose a new random direction.
  if (random(0, 100) < 12) {
    dotDX = random(0, 2) ? 1 : -1;
    dotDY = random(0, 2) ? 1 : -1;
  }

  // After several edge bounces, jump to a new position.
  if (bounceCount >= 8) {
    dotX = random(0, 32);
    dotY = random(0, 8);

    dotDX = random(0, 2) ? 1 : -1;
    dotDY = random(0, 2) ? 1 : -1;

    bounceCount = 0;
  }
}

// --------------------------------------------------
// IDLE ANIMATION LOOP
// --------------------------------------------------

void updateIdle() {
  // Stay blank unless a pattern was explicitly requested.
  if (!patternsRequested || message[0] != '\0') {
    return;
  }

  uint32_t now = millis();

  // Approximately 10 animation frames per second.
  if (now - lastFrame < 100) {
    return;
  }

  lastFrame = now;
  frameNumber++;

  // In random mode, select a pattern every five seconds.
  if (idleMode == 0) {
    if (lastPatternChange == 0 ||
        now - lastPatternChange >= 5000) {

      activePattern = random(1, 8);
      lastPatternChange = now;

      // Reset the dot when random mode selects it.
      if (activePattern == 1) {
        dotX = random(0, 32);
        dotY = random(0, 8);
        dotDX = random(0, 2) ? 1 : -1;
        dotDY = random(0, 2) ? 1 : -1;
        bounceCount = 0;
      }

      // Reset the stick men when random mode selects them.
      if (activePattern == 6) {
        initStickMen();
      }

      Serial.printf("[RANDOM PATTERN] Playing %u\n", activePattern);
    }
  } else {
    activePattern = idleMode;
  }

  // Move the dot only when its pattern is active.
  if (activePattern == 1) {
    updateBouncingDot();
  }

  // Move the stick men only when their pattern is active.
  if (activePattern == 6) {
    updateStickMen();
  }

  drawIdlePattern(activePattern);
}

// --------------------------------------------------
// SETUP
// --------------------------------------------------

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("[STARTUP] LED Matrix Display");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, BUZZER_OFF);

  randomSeed(analogRead(A0) + micros());

  display.begin();
  matrix = display.getGraphicObject();

  display.setIntensity(brightness);
  display.displayClear();

  // Always start blank.
  message[0] = '\0';
  patternsRequested = true;

  initStickMen();

  WiFi.mode(WIFI_STA);

  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);
  configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("[CLOCK] NTP time sync requested.");

  // Automatically start the time/date sequence after every NodeMCU restart.
  // V6 can still be used manually to turn clock/date mode off or on.
  clockModeEnabled = true;
  patternsRequested = false;
  message[0] = '\0';
  startClockFace(false);

  Serial.println("[SYSTEM] Ready. Clock/date sequence started.");
}

// --------------------------------------------------
// MAIN LOOP
// --------------------------------------------------

void loop() {
  Blynk.run();

  checkAlarm();

  if (alarmRinging) {
    updateAlarmRinging();
  } else if (clockModeEnabled) {
    updateClockDisplay();
  } else if (message[0] != '\0') {
    // Keep scrolling text animated.
    if (display.displayAnimate()) {
      display.displayReset();
    }
  } else {
    // Animate only when explicitly requested.
    updateIdle();
  }
}
