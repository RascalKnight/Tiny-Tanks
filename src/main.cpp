/*
 * TINY TANKS - Player 2 (Device B)
 * Display: 2.4" TFT Arduino Shield (Parallel MCUFRIEND)
 * Communication: ESP-NOW (wireless, no router)
 */

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h> // Swapped library
#include <WiFi.h>
#include <esp_now.h>

// ════════════════════════════════════════════
//  CONFIGURATION — CHANGE PER DEVICE
// ════════════════════════════════════════════

#define PLAYER_ID   2    // Set to 2 for Device B

// Paste the OTHER ESP32's MAC address here (Device A's MAC)
uint8_t PEER_MAC[] = {0x28, 0x56, 0x2F, 0x4A, 0x05, 0xA4};

// ════════════════════════════════════════════
//  PIN DEFINITIONS (For Joystick & Button)
// ════════════════════════════════════════════

#define JOY_X     34
#define JOY_Y     35
#define BTN_FIRE  21   

// ════════════════════════════════════════════
//  GAME CONSTANTS (Scaled up for 320x240 screen)
// ════════════════════════════════════════════

#define SCREEN_W     320   // Scaled from 128
#define SCREEN_H     240   // Scaled from 128
#define TANK_SIZE    12    // Scaled from 5
#define BULLET_SIZE   4    // Scaled from 2
#define TANK_SPEED    4    // Increased slightly for larger area
#define BULLET_SPEED  7    // Increased slightly for larger area
#define MAX_BULLETS   3
#define WALL_COUNT    6
#define MAX_HP        3
#define TICK_MS      40   // ~25fps

#define COL_BG       0x0000
#define COL_P1       0x07E0
#define COL_P2       0xF800
#define COL_BULLET1  0x07FF
#define COL_BULLET2  0xFFE0
#define COL_WALL     0x8410
#define COL_TEXT     0xFFFF
#define COL_HP       0xF800

// ════════════════════════════════════════════
//  DATA STRUCTURES
// ════════════════════════════════════════════

typedef struct {
  int16_t jx;
  int16_t jy;
  bool    fire;
  uint8_t hp;
  uint8_t gameState; // 0 = Playing, 1 = P1 Wins, 2 = P2 Wins, 3 = Reset Match
} GamePacket;

typedef struct {
  float x, y;
  float dx, dy;
  int8_t hp;
  bool alive;
} Tank;

typedef struct {
  float x, y;
  float vx, vy;
  bool active;
  uint8_t owner;
} Bullet;

typedef struct {
  int16_t x, y, w, h;
} Wall;

// ════════════════════════════════════════════
//  GLOBALS
// ════════════════════════════════════════════

int center_x = 2048; 
int center_y = 2048; 

MCUFRIEND_kbv tft; // Uses standard 8-bit parallel pin definition implicitly

Tank    tanks[2];
Bullet  bullets[MAX_BULLETS * 2];
Wall    walls[WALL_COUNT];

GamePacket outPkt;
GamePacket peerPkt;
volatile bool peerUpdated = false;

bool prevFireBtn = false;
bool gameOver    = false;
uint8_t winner   = 0;

// ════════════════════════════════════════════
//  FORWARD DECLARATIONS
// ════════════════════════════════════════════
void initGame();
void showSplash();
void drawArena();
void drawHUD();
void drawTank(Tank &t, uint16_t col);
void eraseTank(Tank &t);
void drawBullet(Bullet &b);
void eraseBullet(Bullet &b);
void readLocalInput();
void moveTank(Tank &t, int jx, int jy);
void tryFire(Tank &t, uint8_t owner);
void updateBullets();
bool collidesWall(float nx, float ny);
bool bulletHitsWall(float bx, float by);
bool isButtonPressed();

// ════════════════════════════════════════════
//  ESP-NOW CALLBACKS
// ════════════════════════════════════════════

void onReceive(const uint8_t *mac, const uint8_t *data, int len) {
  if (len == sizeof(GamePacket)) {
    memcpy(&peerPkt, data, sizeof(GamePacket));
    peerUpdated = true;
  }
}

void onSend(const uint8_t *mac, esp_now_send_status_t status) {}

// ════════════════════════════════════════════
//  DEBOUNCED BUTTON READ
// ════════════════════════════════════════════

bool isButtonPressed() {
  if (digitalRead(BTN_FIRE) == LOW) {
    delay(20); 
    if (digitalRead(BTN_FIRE) == LOW) {
      return true;
    }
  }
  return false;
}

// ════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════

void setup() {
  Serial.begin(9600);

  // Initialize MCUFRIEND Shield
  uint16_t identifier = tft.readID();
  if (identifier == 0x00D3 || identifier == 0x0000) {
    identifier = 0x9341; // Force common driver if identification fails
  }
  tft.begin(identifier);
  tft.setRotation(1); // 1 = Landscape orientation (320x240)
  tft.fillScreen(COL_BG);

  pinMode(BTN_FIRE, INPUT_PULLUP);
  analogReadResolution(12);

  WiFi.mode(WIFI_STA);
  Serial.print("My MAC: ");
  Serial.println(WiFi.macAddress());

  delay(100); 
  center_x = analogRead(JOY_X);
  center_y = analogRead(JOY_Y);

  if (esp_now_init() != ESP_OK) {
    tft.setCursor(5, 120);
    tft.setTextColor(COL_HP);
    tft.setTextSize(2);
    tft.print("ESP-NOW FAIL");
    while (1);
  }
  esp_now_register_recv_cb(onReceive);
  esp_now_register_send_cb(onSend);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, PEER_MAC, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  initGame();
  showSplash();
}

// ════════════════════════════════════════════
//  GAME INIT & SPLASH
// ════════════════════════════════════════════

void initGame() {
  // Balanced spawning locations for 320x240 resolution
  tanks[0] = {25, 130, 1, 0, MAX_HP, true};
  tanks[1] = {295, 130, -1, 0, MAX_HP, true};

  for (int i = 0; i < MAX_BULLETS * 2; i++) bullets[i].active = false;

  // Re-scaled wall barriers to match the larger 320x240 battlefield
  walls[0] = {75, 40,  10, 60};
  walls[1] = {235, 40,  10, 60};
  walls[2] = {75, 150,  10, 60};
  walls[3] = {235, 150,  10, 60};
  walls[4] = {130, 105, 60,  10};
  walls[5] = {130, 145, 60,  10};

  gameOver = false;
  winner   = 0;
}

void showSplash() {
  tft.fillScreen(COL_BG);
  tft.setTextSize(4); // Increased text sizes for visual clarity
  tft.setTextColor(COL_P1);
  tft.setCursor(60, 60);
  tft.print("TINY");
  tft.setTextColor(COL_P2);
  tft.setCursor(160, 60);
  tft.print("TANKS");
  
  tft.setTextSize(2);
  tft.setTextColor(COL_TEXT);
  tft.setCursor(50, 150);
  tft.print("Press Fire to start");

  while (!isButtonPressed()) delay(10);
  while (isButtonPressed()) delay(10);
  
  delay(100);
  drawArena();
  drawHUD();
}

// ════════════════════════════════════════════
//  DRAWING
// ════════════════════════════════════════════

void drawArena() {
  tft.fillScreen(COL_BG);
  tft.drawRect(0, 0, SCREEN_W, SCREEN_H, COL_WALL);
  for (int i = 0; i < WALL_COUNT; i++) {
    tft.fillRect(walls[i].x, walls[i].y, walls[i].w, walls[i].h, COL_WALL);
  }
}

void eraseTank(Tank &t) {
  if (!t.alive) return;
  tft.fillRect((int)t.x - TANK_SIZE/2, (int)t.y - TANK_SIZE/2, TANK_SIZE, TANK_SIZE, COL_BG);
  int bx = (int)t.x + (int)(t.dx * 8); // Barrel offset scaled up
  int by = (int)t.y + (int)(t.dy * 8);
  tft.fillRect(bx - 2, by - 2, 4, 4, COL_BG);
}

void drawTank(Tank &t, uint16_t col) {
  if (!t.alive) return;
  tft.fillRect((int)t.x - TANK_SIZE/2, (int)t.y - TANK_SIZE/2, TANK_SIZE, TANK_SIZE, col);
  int bx = (int)t.x + (int)(t.dx * 8); 
  int by = (int)t.y + (int)(t.dy * 8);
  tft.fillRect(bx - 2, by - 2, 4, 4, col);
}

void eraseBullet(Bullet &b) {
  tft.fillRect((int)b.x - BULLET_SIZE/2, (int)b.y - BULLET_SIZE/2, BULLET_SIZE, BULLET_SIZE, COL_BG);
}

void drawBullet(Bullet &b) {
  uint16_t col = (b.owner == 1) ? COL_BULLET1 : COL_BULLET2;
  tft.fillRect((int)b.x - BULLET_SIZE/2, (int)b.y - BULLET_SIZE/2, BULLET_SIZE, BULLET_SIZE, col);
}

void drawHUD() {
  // Clear layout zone at top of screen
  tft.fillRect(2, 2, SCREEN_W - 4, 18, COL_BG);
  tft.setTextColor(COL_P1);
  tft.setTextSize(2);
  tft.setCursor(10, 4);
  tft.print("P1:");
  // Render HP Blocks scaled up
  for (int i = 0; i < tanks[0].hp; i++) tft.fillRect(50 + i*14, 6, 10, 10, COL_P1);

  tft.setTextColor(COL_P2);
  tft.setCursor(200, 4);
  tft.print("P2:");
  for (int i = 0; i < tanks[1].hp; i++) tft.fillRect(240 + i*14, 6, 10, 10, COL_P2);
}

// ════════════════════════════════════════════
//  INPUT RETRIEVAL
// ════════════════════════════════════════════

void readLocalInput() {
  int raw_x = analogRead(JOY_X);
  int raw_y = analogRead(JOY_Y);
  int deadzone = 150;  
  
  int jx = 0;
  int jy = 0;

  if (raw_x < (center_x - deadzone)) {
    jx = map(raw_x, 0, center_x - deadzone, -100, 0);
  } else if (raw_x > (center_x + deadzone)) {
    jx = map(raw_x, center_x + deadzone, 4095, 0, 100);
  }

  if (raw_y < (center_y - deadzone)) {
    jy = map(raw_y, 0, center_y - deadzone, -100, 0);
  } else if (raw_y > (center_y + deadzone)) {
    jy = map(raw_y, center_y + deadzone, 4095, 0, 100);
  }

  bool fireNow = isButtonPressed();

  outPkt.jx   = jx;
  outPkt.jy   = jy;
  outPkt.fire = fireNow && !prevFireBtn;
  outPkt.hp   = tanks[PLAYER_ID - 1].hp;
  
  if (gameOver) {
    outPkt.gameState = winner;
  } else {
    outPkt.gameState = 0;
  }

  prevFireBtn = fireNow;
}

// ════════════════════════════════════════════
//  PHYSICS ENGINE
// ════════════════════════════════════════════

bool collidesWall(float nx, float ny) {
  int hw = TANK_SIZE / 2;
  // Border limits adapted for 320x240 layout (HUD height padding added)
  if (nx - hw < 2 || nx + hw > SCREEN_W - 3) return true;
  if (ny - hw < 20 || ny + hw > SCREEN_H - 3) return true;
  for (int i = 0; i < WALL_COUNT; i++) {
    if (nx + hw > walls[i].x && nx - hw < walls[i].x + walls[i].w &&
        ny + hw > walls[i].y && ny - hw < walls[i].y + walls[i].h) return true;
  }
  return false;
}

bool bulletHitsWall(float bx, float by) {
  if (bx < 2 || bx > SCREEN_W - 3) return true;
  if (by < 20 || by > SCREEN_H - 3) return true;
  for (int i = 0; i < WALL_COUNT; i++) {
    if (bx > walls[i].x && bx < walls[i].x + walls[i].w &&
        by > walls[i].y && by < walls[i].y + walls[i].h) return true;
  }
  return false;
}

void moveTank(Tank &t, int jx, int jy) {
  if (!t.alive) return;
  
  float nx = t.x;
  float ny = t.y;
  float spd = TANK_SPEED;

  bool wantMoveX = (abs(jx) > 30);
  bool wantMoveY = (abs(jy) > 30);

  if (wantMoveX && !wantMoveY) {
    nx += (jx > 0) ? spd : -spd;
    t.dx = (jx > 0) ? 1 : -1;
    t.dy = 0;
  } 
  else if (wantMoveY && !wantMoveX) {
    ny += (jy > 0) ? spd : -spd;
    t.dy = (jy > 0) ? 1 : -1;
    t.dx = 0;
  }
  else if (wantMoveX && wantMoveY) {
    if (abs(jx) > abs(jy)) {
      nx += (jx > 0) ? spd : -spd;
      t.dx = (jx > 0) ? 1 : -1;
      t.dy = 0;
    } else {
      ny += (jy > 0) ? spd : -spd;
      t.dy = (jy > 0) ? 1 : -1;
      t.dx = 0;
    }
  }

  if (!collidesWall(nx, ny)) {
    t.x = nx; 
    t.y = ny;
  }
}

void tryFire(Tank &t, uint8_t owner) {
  int base = (owner == 1) ? 0 : MAX_BULLETS;
  for (int i = base; i < base + MAX_BULLETS; i++) {
    if (!bullets[i].active) {
      bullets[i] = {t.x + t.dx * 10, t.y + t.dy * 10,
                    t.dx * BULLET_SPEED, t.dy * BULLET_SPEED,
                    true, owner};
      return;
    }
  }
}

void updateBullets() {
  for (int i = 0; i < MAX_BULLETS * 2; i++) {
    if (!bullets[i].active) continue;
    eraseBullet(bullets[i]);
    bullets[i].x += bullets[i].vx;
    bullets[i].y += bullets[i].vy;

    if (bulletHitsWall(bullets[i].x, bullets[i].y)) {
      bullets[i].active = false;
      continue;
    }

    uint8_t target = (bullets[i].owner == 1) ? 1 : 0;
    Tank &t = tanks[target];
    if (t.alive &&
        abs(bullets[i].x - t.x) < TANK_SIZE &&
        abs(bullets[i].y - t.y) < TANK_SIZE) {
      bullets[i].active = false;
      t.hp--;
      if (t.hp <= 0) {
        t.alive = false;
        gameOver = true;
        winner   = bullets[i].owner;
        tft.fillScreen(COL_BG);
      }
    } else {
      drawBullet(bullets[i]);
    }
  }
}

// ════════════════════════════════════════════
//  MAIN EXECUTION LOOP
// ════════════════════════════════════════════

void loop() {
  uint32_t t0 = millis();

  if (peerUpdated) {
    peerUpdated = false;
    uint8_t peerId = (PLAYER_ID == 1) ? 2 : 1;
    int peerIdx = 2 - PLAYER_ID;

    if (peerPkt.gameState == 1 || peerPkt.gameState == 2) {
      if (!gameOver) {
        gameOver = true;
        winner = peerPkt.gameState;
        tft.fillScreen(COL_BG);
      }
    } 
    else if (peerPkt.gameState == 3) {
      if (gameOver) {
        initGame();
        drawArena();
        drawHUD();
        return;
      }
    }

    if (!gameOver) {
      eraseTank(tanks[peerIdx]);
      moveTank(tanks[peerIdx], peerPkt.jx, peerPkt.jy);
      if (peerPkt.fire) tryFire(tanks[peerIdx], peerId);
      tanks[peerIdx].hp = peerPkt.hp;
    }
  }

  if (gameOver) {
    tft.setTextSize(4);
    tft.setCursor(60, 70);
    uint16_t wCol = (winner == 1) ? COL_P1 : COL_P2;
    tft.setTextColor(wCol);
    tft.print("P"); tft.print(winner);
    tft.println(" WINS!");
    tft.setTextSize(2);
    tft.setTextColor(COL_TEXT);
    tft.setCursor(50, 140);
    tft.print("Press FIRE to replay");

    bool fireNow = isButtonPressed();
    bool clicked = fireNow && !prevFireBtn;
    prevFireBtn = fireNow;

    if (clicked) {
      outPkt.gameState = 3; 
      esp_now_send(PEER_MAC, (uint8_t *)&outPkt, sizeof(outPkt));
      delay(50);
      initGame();
      drawArena();
      drawHUD();
      return;
    }

    outPkt.gameState = winner;
    esp_now_send(PEER_MAC, (uint8_t *)&outPkt, sizeof(outPkt));

  } else {
    readLocalInput();
    esp_now_send(PEER_MAC, (uint8_t *)&outPkt, sizeof(outPkt));

    int myIdx = PLAYER_ID - 1;
    eraseTank(tanks[myIdx]);
    moveTank(tanks[myIdx], outPkt.jx, outPkt.jy);
    if (outPkt.fire) tryFire(tanks[myIdx], PLAYER_ID);

    updateBullets();

    drawTank(tanks[0], COL_P1);
    drawTank(tanks[1], COL_P2);
    drawHUD();
  }

  uint32_t elapsed = millis() - t0;
  if (elapsed < TICK_MS) delay(TICK_MS - elapsed);
}