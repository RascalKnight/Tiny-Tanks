/*
 * ╔══════════════════════════════════════════════════════╗
 * ║               T I N Y   T A N K S                   ║
 * ║   2.4" TFT Shield  ·  MCUFRIEND_kbv  ·  320×240     ║
 * ║   Wireless 2-player via ESP-NOW (no router)          ║
 * ╚══════════════════════════════════════════════════════╝
 */

#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>
#include <WiFi.h>
#include <esp_now.h>

// ┌─────────────────────────────────────────────────────┐
// │  CHANGE THESE TWO THINGS PER DEVICE                 │
// └─────────────────────────────────────────────────────┘
#define PLAYER_ID  1
uint8_t PEER_MAC[] = {0x28, 0x56, 0x2F, 0x4A, 0x05, 0xA4};

// ┌─────────────────────────────────────────────────────┐
// │  PINS                                               │
// └─────────────────────────────────────────────────────┘
#define PIN_JOY_X   34
#define PIN_JOY_Y   35
#define PIN_FIRE    21   // GPIO 21, supports INPUT_PULLUP

// ┌─────────────────────────────────────────────────────┐
// │  DISPLAY  (320 wide × 240 tall, landscape)          │
// └─────────────────────────────────────────────────────┘
#define SCR_W  320
#define SCR_H  240
#define HUD_H   14

// ┌─────────────────────────────────────────────────────┐
// │  GAME TUNING                                        │
// └─────────────────────────────────────────────────────┘
#define TANK_HALF    5
#define BARREL_LEN   7
#define BULLET_R     2
#define TANK_SPD     3
#define BULLET_SPD   6
#define MAX_BULLETS  3
#define NUM_WALLS    6
#define MAX_HP       3
#define FPS_MS      40     // ~25 fps
#define JOY_DEAD    30

// How long before FIRE is accepted on the win splash
#define WIN_LOCKOUT_MS  3000

// ┌─────────────────────────────────────────────────────┐
// │  PACKET STATES                                      │
// └─────────────────────────────────────────────────────┘
#define STATE_PLAY   0
#define STATE_P1WIN  1
#define STATE_P2WIN  2
#define STATE_READY  3   // post-win: "I pressed FIRE, ready for rematch"
#define STATE_LOBBY  4   // pre-game: "I pressed FIRE on the splash screen"

// ┌─────────────────────────────────────────────────────┐
// │  PALETTE  (RGB-565)                                 │
// └─────────────────────────────────────────────────────┘
#define C_BG    0x0000
#define C_WALL  0x4A49
#define C_HUD   0x2104
#define C_P1    0x07E0   // green
#define C_P2    0xFD20   // orange-red
#define C_B1    0x07FF   // cyan
#define C_B2    0xFFE0   // yellow
#define C_WHITE 0xFFFF
#define C_GREY  0x8410
#define C_WARN  0xFD20   // reuse orange for warnings

// ┌─────────────────────────────────────────────────────┐
// │  DATA STRUCTURES                                    │
// └─────────────────────────────────────────────────────┘
struct Packet {
  int16_t jx, jy;
  bool    fire;
  uint8_t hp;
  uint8_t state;
};

struct Tank {
  float  x, y;
  float  dx, dy;
  int8_t hp;
  bool   alive;
};

struct Bullet {
  float   x, y, vx, vy;
  bool    active;
  uint8_t owner;
};

struct Wall {
  int16_t x, y, w, h;
};

// ┌─────────────────────────────────────────────────────┐
// │  GLOBALS                                            │
// └─────────────────────────────────────────────────────┘
MCUFRIEND_kbv tft;

Tank   tank[2];
Bullet bullet[MAX_BULLETS * 2];
Wall   wall[NUM_WALLS];

// Double buffering for thread-safe packet sharing
Packet outPkt;
Packet inPkt;          // Safe packet read by the main loop
Packet inPktShared;    // Shared packet written by ISR callback
volatile bool gotPacket = false;
portMUX_TYPE pktMux = portMUX_INITIALIZER_UNLOCKED;

int  joyXCenter = 2048;
int  joyYCenter = 2048;
bool prevFire   = false;

// Cache to prevent redrawing HUD unless values change
int8_t lastHUDHp[2] = {-1, -1};

// ── Game state ──────────────────────────────────────────
bool    gameOver = false;
uint8_t winner   = 0;

// ┌─────────────────────────────────────────────────────┐
// │  ESP-NOW                                            │
// └─────────────────────────────────────────────────────┘
// ESP32 Arduino Core SDK version check for compatibility
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
void onRecv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
#else
void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  if (len == sizeof(Packet)) {
    portENTER_CRITICAL_ISR(&pktMux);
    memcpy(&inPktShared, data, sizeof(Packet));
    gotPacket = true;
    portEXIT_CRITICAL_ISR(&pktMux);
  }
}

void onSent(const uint8_t*, esp_now_send_status_t) {}

void sendPacket() {
  esp_now_send(PEER_MAC, (uint8_t*)&outPkt, sizeof(Packet));
}

// ┌─────────────────────────────────────────────────────┐
// │  BUTTON                                             │
// └─────────────────────────────────────────────────────┘
bool firePressed() {
  if (digitalRead(PIN_FIRE) == LOW) {
    delay(18); // Simple debounce
    return digitalRead(PIN_FIRE) == LOW;
  }
  return false;
}

// ┌─────────────────────────────────────────────────────┐
// │  WALLS                                              │
// └─────────────────────────────────────────────────────┘
void layoutWalls() {
  wall[0] = {  70,  60, 10, 55 };
  wall[1] = { 240,  60, 10, 55 };
  wall[2] = {  70, 125, 10, 55 };
  wall[3] = { 240, 125, 10, 55 };
  wall[4] = { 148, 100, 24, 10 };
  wall[5] = { 148, 130, 24, 10 };
}

// ┌─────────────────────────────────────────────────────┐
// │  RESET                                              │
// └─────────────────────────────────────────────────────┘
void resetGame() {
  tank[0] = { 25,  120,  1, 0, MAX_HP, true };
  tank[1] = { 294, 120, -1, 0, MAX_HP, true };
  for (int i = 0; i < MAX_BULLETS * 2; i++) bullet[i].active = false;

  gameOver = false;
  winner   = 0;
  lastHUDHp[0] = -1;
  lastHUDHp[1] = -1;
}

// ┌─────────────────────────────────────────────────────┐
// │  COLLISION                                          │
// └─────────────────────────────────────────────────────┘
bool tankHitsWall(float nx, float ny) {
  if (nx - TANK_HALF < 1)          return true;
  if (nx + TANK_HALF > SCR_W - 2)  return true;
  if (ny - TANK_HALF < HUD_H + 2)  return true;
  if (ny + TANK_HALF > SCR_H - 2)  return true;
  for (int i = 0; i < NUM_WALLS; i++)
    if (nx + TANK_HALF > wall[i].x             &&
        nx - TANK_HALF < wall[i].x + wall[i].w &&
        ny + TANK_HALF > wall[i].y             &&
        ny - TANK_HALF < wall[i].y + wall[i].h) return true;
  return false;
}

bool bulletHitsWall(float bx, float by) {
  if (bx < 1 || bx > SCR_W - 2)      return true;
  if (by < HUD_H || by > SCR_H - 2)  return true;
  for (int i = 0; i < NUM_WALLS; i++)
    if (bx > wall[i].x && bx < wall[i].x + wall[i].w &&
        by > wall[i].y && by < wall[i].y + wall[i].h) return true;
  return false;
}

// ┌─────────────────────────────────────────────────────┐
// │  DRAW HELPERS                                       │
// └─────────────────────────────────────────────────────┘
void clearTank(Tank& t) {
  if (!t.alive) return;
  int x = (int)t.x, y = (int)t.y;
  tft.fillRect(x - TANK_HALF, y - TANK_HALF, TANK_HALF*2, TANK_HALF*2, C_BG);
  tft.fillRect(x + (int)(t.dx*BARREL_LEN) - 2,
               y + (int)(t.dy*BARREL_LEN) - 2, 4, 4, C_BG);
}

void paintTank(Tank& t, uint16_t col) {
  if (!t.alive) return;
  int x = (int)t.x, y = (int)t.y;
  tft.fillRect(x - TANK_HALF, y - TANK_HALF, TANK_HALF*2, TANK_HALF*2, col);
  tft.drawFastHLine(x - TANK_HALF+1, y - TANK_HALF+1, TANK_HALF*2-2, col | 0x8410);
  tft.fillRect(x + (int)(t.dx*BARREL_LEN) - 2,
               y + (int)(t.dy*BARREL_LEN) - 2, 4, 4, col);
}

void clearBullet(Bullet& b) {
  tft.fillCircle((int)b.x, (int)b.y, BULLET_R, C_BG);
}

void paintBullet(Bullet& b) {
  tft.fillCircle((int)b.x, (int)b.y, BULLET_R, (b.owner == 1) ? C_B1 : C_B2);
}

void drawWalls() {
  for (int i = 0; i < NUM_WALLS; i++) {
    tft.fillRect(wall[i].x, wall[i].y, wall[i].w, wall[i].h, C_WALL);
    tft.drawRect (wall[i].x, wall[i].y, wall[i].w, wall[i].h, C_GREY);
  }
}

// Draws the static UI structures (drawn once per game start)
void drawStaticHUD() {
  tft.fillRect(0, 0, SCR_W, HUD_H, C_HUD);
  tft.drawFastHLine(0, HUD_H, SCR_W, C_WALL);

  tft.setTextSize(1);
  tft.setTextColor(C_P1, C_HUD);
  tft.setCursor(4, 3);
  tft.print("P1");

  tft.setTextColor(C_P2, C_HUD);
  tft.setCursor(255, 3);
  tft.print("P2");

  tft.setTextColor(C_GREY, C_HUD);
  tft.setCursor(153, 3);
  tft.print("VS");
}

// Only redraws the HP blocks if HP changes (high performance)
void drawHUD() {
  // P1 HP Bar
  if (tank[0].hp != lastHUDHp[0]) {
    for (int i = 0; i < MAX_HP; i++) {
      tft.fillRect(18 + i*10, 3, 8, 8, (i < tank[0].hp) ? C_P1 : C_HUD);
    }
    lastHUDHp[0] = tank[0].hp;
  }

  // P2 HP Bar
  if (tank[1].hp != lastHUDHp[1]) {
    for (int i = 0; i < MAX_HP; i++) {
      tft.fillRect(271 + i*10, 3, 8, 8, (i < tank[1].hp) ? C_P2 : C_HUD);
    }
    lastHUDHp[1] = tank[1].hp;
  }
}

void drawArena() {
  tft.fillScreen(C_BG);
  tft.drawRect(0, HUD_H, SCR_W, SCR_H - HUD_H, C_WALL);
  drawWalls();
  drawStaticHUD();
  drawHUD();
}

// ┌─────────────────────────────────────────────────────┐
// │  SPLASH / LOBBY                                     │
// └─────────────────────────────────────────────────────┘
void showSplash() {
  tft.fillScreen(C_BG);
  tft.fillRect(0,   0, SCR_W,  6, C_P1);
  tft.fillRect(0, 234, SCR_W,  6, C_P2);

  tft.setTextSize(4);
  tft.setTextColor(C_P1);
  tft.setCursor(76, 50);
  tft.print("TINY");
  tft.setTextColor(C_P2);
  tft.setCursor(60, 98);
  tft.print("TANKS");

  tft.setTextSize(1);
  tft.setTextColor(C_GREY);
  tft.setCursor(95, 158);
  tft.print("2-PLAYER  \xb7  ESP-NOW");

  bool iReady = false;
  bool peerReady = false;
  
  // Cache to track and trigger UI updates only on state changes
  bool lastIReady = !iReady;
  bool lastPeerReady = !peerReady;

  // Consume any button already held at boot
  while (firePressed()) delay(10);
  prevFire = false;

  while (true) {
    // ── Check if any states changed since last loop ─────────
    bool stateChanged = (iReady != lastIReady) || (peerReady != lastPeerReady);

    if (stateChanged) {
      tft.fillRect(0, 178, SCR_W, 60, C_BG); // Clear text area only once on state changes

      // Draw local readiness prompt
      tft.setTextSize(1);
      if (!iReady) {
        tft.setTextColor(C_WHITE);
        tft.setCursor(103, 182);
        tft.print("Press FIRE to ready up");
      } else {
        tft.setTextColor(C_GREY);
        tft.setCursor(118, 182);
        tft.print("Waiting for peer...");
      }

      // ── P1 / P2 ready pips ───────────────────────────
      bool p1Ready = (PLAYER_ID == 1) ? iReady : peerReady;
      bool p2Ready = (PLAYER_ID == 2) ? iReady : peerReady;

      tft.setTextColor(p1Ready ? C_P1 : C_GREY);
      tft.setCursor(80, 210);
      tft.print(p1Ready ? "P1  READY" : "P1  ......");

      tft.setTextColor(p2Ready ? C_P2 : C_GREY);
      tft.setCursor(190, 210);
      tft.print(p2Ready ? "P2  READY" : "P2  ......");

      lastIReady = iReady;
      lastPeerReady = peerReady;
    }

    // ── Local button ─────────────────────────────────
    bool fireNow = firePressed();
    if (fireNow && !prevFire && !iReady) {
      iReady = true;
    }
    prevFire = fireNow;

    // ── Broadcast our lobby state ────────────────────
    outPkt = { 0, 0, false, (uint8_t)MAX_HP, STATE_LOBBY };
    sendPacket();

    // ── Check for peer response (thread-safe copy) ───
    bool localGotPacket = false;
    Packet localInPkt;
    if (gotPacket) {
      portENTER_CRITICAL(&pktMux);
      localInPkt = inPktShared;
      localGotPacket = true;
      gotPacket = false;
      portEXIT_CRITICAL(&pktMux);
    }

    if (localGotPacket) {
      if (localInPkt.state == STATE_LOBBY || localInPkt.state == STATE_READY) {
        peerReady = true;
      }
    }

    // ── Both ready → enter game ───────────────────────
    if (iReady && peerReady) {
      tft.fillRect(0, 178, SCR_W, 60, C_BG);
      tft.setTextSize(2);
      tft.setTextColor(C_WHITE);
      tft.setCursor(100, 195);
      tft.print("GAME START!");
      delay(600);
      break;
    }

    delay(80); // ~12 checks/sec
  }

  // Flush button
  while (firePressed()) delay(10);
  prevFire = false;

  drawArena();
}

// ┌─────────────────────────────────────────────────────┐
// │  WIN SPLASH                                         │
// └─────────────────────────────────────────────────────┘
void showWinSplash() {
  uint16_t wCol     = (winner == 1) ? C_P1 : C_P2;
  uint16_t loserCol = (winner == 1) ? C_P2 : C_P1;

  tft.fillScreen(C_BG);
  tft.fillRect(0,   0, SCR_W,  6, wCol);
  tft.fillRect(0, 234, SCR_W,  6, loserCol);

  tft.setTextSize(3);
  tft.setTextColor(wCol);
  tft.setCursor(28, 48);
  tft.print("PLAYER ");
  tft.print(winner);
  tft.setTextSize(2);
  tft.setCursor(98, 95);
  tft.print("WINS!");

  tft.drawFastHLine(40, 125, SCR_W - 80, C_GREY);

  while (firePressed()) delay(10);
  prevFire = false;

  uint32_t lockoutStart = millis();
  bool iReady    = false;
  bool peerReady = false;

  int  lastCountdown = -1;
  bool lastP1Ready = !iReady; // caches to prevent spam redrawing
  bool lastP2Ready = !peerReady;
  bool lastInLockout = true;

  while (true) {
    uint32_t elapsed   = millis() - lockoutStart;
    bool     inLockout = elapsed < WIN_LOCKOUT_MS;
    int      countdown = inLockout
                         ? (int)((WIN_LOCKOUT_MS - elapsed + 999) / 1000)
                         : 0;

    bool p1Ready = (PLAYER_ID == 1) ? iReady : peerReady;
    bool p2Ready = (PLAYER_ID == 2) ? iReady : peerReady;

    // Check if drawing updates are necessary
    bool statusChanged = (countdown != lastCountdown) || 
                         (inLockout != lastInLockout) ||
                         (p1Ready != lastP1Ready) || 
                         (p2Ready != lastP2Ready);

    if (statusChanged) {
      tft.fillRect(0, 138, SCR_W, 102, C_BG);

      if (inLockout) {
        tft.setTextSize(2);
        tft.setTextColor(C_GREY);
        tft.setCursor(88, 148);
        tft.print("Rematch in  ");
        tft.print(countdown);
      } else {
        tft.setTextSize(1);
        tft.setTextColor(p1Ready ? C_P1 : C_GREY);
        tft.setCursor(60, 148);
        tft.print(p1Ready ? "P1  READY" : "P1  ......");

        tft.setTextColor(p2Ready ? C_P2 : C_GREY);
        tft.setCursor(200, 148);
        tft.print(p2Ready ? "P2  READY" : "P2  ......");

        tft.setTextColor(C_WHITE);
        tft.setCursor(88, 195);
        tft.print("Press FIRE to rematch");
      }

      lastCountdown = countdown;
      lastInLockout = inLockout;
      lastP1Ready = p1Ready;
      lastP2Ready = p2Ready;
    }

    // ── Button (only after lockout) ───────────────────
    if (!inLockout) {
      bool fireNow = firePressed();
      bool clicked = fireNow && !prevFire;
      prevFire     = fireNow;
      if (clicked && !iReady) iReady = true;
    } else {
      prevFire = firePressed();
    }

    // ── Broadcast state ───────────────────────────────
    outPkt.state = iReady ? STATE_READY : (uint8_t)winner;
    outPkt.hp    = tank[PLAYER_ID - 1].hp;
    sendPacket();

    // ── Check peer (thread-safe copy) ────────────────
    bool localGotPacket = false;
    Packet localInPkt;
    if (gotPacket) {
      portENTER_CRITICAL(&pktMux);
      localInPkt = inPktShared;
      localGotPacket = true;
      gotPacket = false;
      portEXIT_CRITICAL(&pktMux);
    }

    if (localGotPacket) {
      if (localInPkt.state == STATE_READY) peerReady = true;
    }

    // ── Both ready → break ────────────────────────────
    if (iReady && peerReady) {
      tft.fillRect(0, 138, SCR_W, 102, C_BG);
      tft.setTextSize(2);
      tft.setTextColor(C_WHITE);
      tft.setCursor(100, 180);
      tft.print("GAME START!");
      delay(600);
      break;
    }

    delay(80);
  }

  while (firePressed()) delay(10);
  prevFire = false;
}

// ┌─────────────────────────────────────────────────────┐
// │  MOVEMENT & FIRING                                  │
// └─────────────────────────────────────────────────────┘
void moveTank(Tank& t, int jx, int jy) {
  if (!t.alive) return;
  bool mx = abs(jx) > JOY_DEAD;
  bool my = abs(jy) > JOY_DEAD;
  if (!mx && !my) return;
  float nx = t.x, ny = t.y;
  if (!my || (mx && abs(jx) >= abs(jy))) {
    nx += (jx > 0) ? TANK_SPD : -TANK_SPD;
    t.dx = (jx > 0) ? 1 : -1; t.dy = 0;
  } else {
    ny += (jy > 0) ? TANK_SPD : -TANK_SPD;
    t.dy = (jy > 0) ? 1 : -1; t.dx = 0;
  }
  if (!tankHitsWall(nx, ny)) { t.x = nx; t.y = ny; }
}

void fireBullet(Tank& t, uint8_t owner) {
  int base = (owner == 1) ? 0 : MAX_BULLETS;
  for (int i = base; i < base + MAX_BULLETS; i++) {
    if (!bullet[i].active) {
      bullet[i] = {
        t.x + t.dx * (TANK_HALF + 3),
        t.y + t.dy * (TANK_HALF + 3),
        t.dx * BULLET_SPD,
        t.dy * BULLET_SPD,
        true, owner
      };
      return;
    }
  }
}

void stepBullets() {
  for (int i = 0; i < MAX_BULLETS * 2; i++) {
    if (!bullet[i].active) continue;
    clearBullet(bullet[i]);
    bullet[i].x += bullet[i].vx;
    bullet[i].y += bullet[i].vy;

    if (bulletHitsWall(bullet[i].x, bullet[i].y)) {
      bullet[i].active = false; continue;
    }

    // --- SELF-DAMAGE AUTHORITY HIT DETECTION ---
    // P1's device only registers damage to P1, and P2's device only registers damage to P2.
    int target = (bullet[i].owner == 1) ? 1 : 0;
    
    // Only check collision if we are the target player
    if (target == (PLAYER_ID - 1)) {
      Tank& tgt = tank[target];
      if (tgt.alive &&
          abs(bullet[i].x - tgt.x) < TANK_HALF + BULLET_R + 1 &&
          abs(bullet[i].y - tgt.y) < TANK_HALF + BULLET_R + 1) {
        bullet[i].active = false;
        tgt.hp--;
        if (tgt.hp <= 0) {
          tgt.alive = false;
          gameOver  = true;
          winner    = bullet[i].owner;
        }
        continue;
      }
    } else {
      // For the peer tank, we check collision to clean up the bullet locally but do NOT apply damage.
      // The peer's device is responsible for applying its own damage and broadcasting its new HP.
      Tank& peerTgt = tank[target];
      if (peerTgt.alive &&
          abs(bullet[i].x - peerTgt.x) < TANK_HALF + BULLET_R + 1 &&
          abs(bullet[i].y - peerTgt.y) < TANK_HALF + BULLET_R + 1) {
        bullet[i].active = false;
        continue;
      }
    }
    paintBullet(bullet[i]);
  }
}

// ┌─────────────────────────────────────────────────────┐
// │  INPUT                                              │
// └─────────────────────────────────────────────────────┘
void readInput() {
  const int dz = 180;
  int rx = analogRead(PIN_JOY_X);
  int ry = analogRead(PIN_JOY_Y);

  int jx = 0, jy = 0;
  if      (rx < joyXCenter - dz) jx = map(rx, 0,               joyXCenter-dz, -100,  0);
  else if (rx > joyXCenter + dz) jx = map(rx, joyXCenter+dz,   4095,             0, 100);
  if      (ry < joyYCenter - dz) jy = map(ry, 0,               joyYCenter-dz, -100,  0);
  else if (ry > joyYCenter + dz) jy = map(ry, joyYCenter+dz,   4095,             0, 100);

  // Apply deadzone to values close to center to eliminate sensor drift
  if (abs(jx) < JOY_DEAD) jx = 0;
  if (abs(jy) < JOY_DEAD) jy = 0;

  bool fireNow  = firePressed();
  outPkt.jx    = jx;
  outPkt.jy    = jy;
  outPkt.fire  = fireNow && !prevFire;
  outPkt.hp    = tank[PLAYER_ID - 1].hp;
  outPkt.state = gameOver ? winner : STATE_PLAY;
  prevFire     = fireNow;
}

// ┌─────────────────────────────────────────────────────┐
// │  SETUP                                              │
// └─────────────────────────────────────────────────────┘
void setup() {
  Serial.begin(115200);
  delay(800);

  uint16_t id = tft.readID();
  Serial.print("Display ID: 0x"); Serial.println(id, HEX);
  if (id == 0x00D3 || id == 0x0000) { Serial.println("Forcing ILI9341"); id = 0x9341; }
  tft.begin(id);
  tft.setRotation(1);
  tft.fillScreen(C_BG);

  pinMode(PIN_FIRE, INPUT_PULLUP);
  analogReadResolution(12);

  delay(120);
  int rawX = analogRead(PIN_JOY_X);
  int rawY = analogRead(PIN_JOY_Y);
  
  // Sanity check joystick center readings to handle startup drift or held joystick
  if (rawX >= 1500 && rawX <= 2500) joyXCenter = rawX;
  else joyXCenter = 2048;
  if (rawY >= 1500 && rawY <= 2500) joyYCenter = rawY;
  else joyYCenter = 2048;

  Serial.printf("Joy centre  X=%d  Y=%d\n", joyXCenter, joyYCenter);

  WiFi.mode(WIFI_STA);
  Serial.print("MAC: "); Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    tft.setTextColor(0xF800); tft.setTextSize(2);
    tft.setCursor(60, 110); tft.print("ESP-NOW FAIL");
    while (true) delay(1000);
  }
  
  // Register callbacks (handling ESP32 Core v3 compatibility)
  #if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    esp_now_register_recv_cb(esp_now_recv_cb_t(onRecv));
  #else
    esp_now_register_recv_cb(onRecv);
  #endif
  esp_now_register_send_cb(onSent);

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, PEER_MAC, 6);
  peer.channel = 0; peer.encrypt = false;
  esp_now_add_peer(&peer);

  layoutWalls();
  resetGame();
  showSplash();   // blocks until both players are ready
}

// ┌─────────────────────────────────────────────────────┐
// │  MAIN LOOP                                          │
// └─────────────────────────────────────────────────────┘
void loop() {
  uint32_t frameStart = millis();

  int myIdx  = PLAYER_ID - 1;
  int peerIdx = 1 - myIdx;
  int peerId  = 3 - PLAYER_ID;

  // ── Process incoming packet (Thread-Safe Double Buffer Copy) ────
  bool localGotPacket = false;
  if (gotPacket) {
    portENTER_CRITICAL(&pktMux);
    inPkt = inPktShared;
    localGotPacket = true;
    gotPacket = false;
    portEXIT_CRITICAL(&pktMux);
  }

  if (localGotPacket) {
    if (!gameOver) {
      if (inPkt.state == STATE_P1WIN || inPkt.state == STATE_P2WIN) {
        gameOver = true;
        winner   = inPkt.state;
      } else {
        clearTank(tank[peerIdx]);
        moveTank(tank[peerIdx], inPkt.jx, inPkt.jy);
        if (inPkt.fire) fireBullet(tank[peerIdx], peerId);
        // Sync peer HP directly from their broadcasted authoritative state
        tank[peerIdx].hp = inPkt.hp;
      }
    }
  }

  // ════════════════════════════════════════════════════
  //  GAME OVER PHASE
  // ════════════════════════════════════════════════════
  if (gameOver) {
    showWinSplash();
    resetGame();
    drawArena();
    return;

  // ════════════════════════════════════════════════════
  //  NORMAL GAMEPLAY PHASE
  // ════════════════════════════════════════════════════
  } else {
    readInput();
    sendPacket();

    clearTank(tank[myIdx]);
    moveTank(tank[myIdx], outPkt.jx, outPkt.jy);
    if (outPkt.fire) fireBullet(tank[myIdx], PLAYER_ID);

    // If a bullet just ended the game, broadcast immediately
    if (gameOver) {
      outPkt.state = winner;
      sendPacket();
    }

    stepBullets();
    paintTank(tank[0], C_P1);
    paintTank(tank[1], C_P2);
    drawHUD(); // Redraws dynamically only when HP state actually changes
  }

  uint32_t tickElapsed = millis() - frameStart;
  if (tickElapsed < FPS_MS) delay(FPS_MS - tickElapsed);
}
