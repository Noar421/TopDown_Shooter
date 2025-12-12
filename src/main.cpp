// ============================================================================
// main.cpp - 90s Arcade Style Top-Down Shooter for ESP32-S3
// ============================================================================

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include "grafx.h"

// ============================================================================
// CONFIGURATION
// ============================================================================

#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 320
#define GAME_FPS 30
#define FRAME_TIME (1000 / GAME_FPS)

// Touch calibration - adjust these for your screen
#define TOUCH_THRESHOLD 10

// Game constants
#define MAX_ENEMIES 20
#define MAX_PLAYER_BULLETS 30
#define MAX_ENEMY_BULLETS 40
#define MAX_POWERUPS 5
#define MAX_EXPLOSIONS 10
#define MAX_PARTICLES 50

// ============================================================================
// LOVYANGFX SETUP - Configure for your ILI9488
// ============================================================================

class LGFX : public lgfx::LGFX_Device
{
  lgfx::Panel_ILI9488 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
  lgfx::Light_PWM _light_instance;
  lgfx::Touch_FT5x06 _touch_instance; // FT6206 uses FT5x06 driver

public:
  LGFX(void)
  {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI3_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 12; // Adjust for your board
      cfg.pin_mosi = 13;
      cfg.pin_miso = 14;
      cfg.pin_dc = 42;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = 3;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.memory_width = 320;
      cfg.memory_height = 480;
      cfg.panel_width = 320;
      cfg.panel_height = 480;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = true;
      cfg.invert = false;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;
      _panel_instance.config(cfg);
    }

    {
      auto cfg = _touch_instance.config();
      cfg.x_min = 0;
      cfg.x_max = 319;
      cfg.y_min = 0;
      cfg.y_max = 479;
      cfg.pin_int = -1;
      cfg.bus_shared = true;
      cfg.offset_rotation = 0;
      cfg.i2c_port = 0;
      cfg.i2c_addr = 0x38;
      cfg.pin_sda = 2;
      cfg.pin_scl = 1;
      cfg.freq = 400000;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }

    setPanel(&_panel_instance);
  }
};

LGFX display;
LGFX_Sprite canvas(&display);

// ============================================================================
// UTILITY STRUCTURES
// ============================================================================

struct Vec2
{
  float x, y;
  Vec2(float x = 0, float y = 0) : x(x), y(y) {}

  Vec2 operator+(const Vec2 &v) const { return Vec2(x + v.x, y + v.y); }
  Vec2 operator-(const Vec2 &v) const { return Vec2(x - v.x, y - v.y); }
  Vec2 operator*(float s) const { return Vec2(x * s, y * s); }
  float length() const { return sqrt(x * x + y * y); }
  Vec2 normalize() const
  {
    float len = length();
    return len > 0 ? Vec2(x / len, y / len) : Vec2(0, 0);
  }
};

struct Rect
{
  float x, y, w, h;
  Rect(float x = 0, float y = 0, float w = 0, float h = 0) : x(x), y(y), w(w), h(h) {}

  bool intersects(const Rect &r) const
  {
    return x < r.x + r.w && x + w > r.x && y < r.y + r.h && y + h > r.y;
  }
};

// ============================================================================
// SOUND SYSTEM
// ============================================================================

class SoundSystem
{
private:
  struct Sound
  {
    int freq;
    int duration;
  };

  Sound currentSound;
  unsigned long soundStartTime;
  bool isPlaying;

public:
  enum SoundEffect
  {
    SHOOT,
    EXPLOSION,
    HIT,
    POWERUP,
    ENEMY_SHOOT
  };

  void init()
  {
    ledcSetup(0, 2000, 8);
    ledcAttachPin(45, 0);
    isPlaying = false;
  }

  void play(SoundEffect effect)
  {
    // // SFW ;-)
    // return;

    switch (effect)
    {
    case SHOOT:
      playTone(1500, 50);
      break;
    case EXPLOSION:
      playTone(300, 200);
      break;
    case HIT:
      playTone(200, 100);
      break;
    case POWERUP:
      playTone(2000, 150);
      break;
    case ENEMY_SHOOT:
      playTone(800, 40);
      break;
    }
  }

  void update()
  {
    if (isPlaying && millis() - soundStartTime > currentSound.duration)
    {
      ledcWriteTone(0, 0);
      isPlaying = false;
    }
  }

private:
  void playTone(int freq, int duration)
  {
    currentSound = {freq, duration};
    soundStartTime = millis();
    isPlaying = true;
    ledcWriteTone(0, freq);
  }
};

SoundSystem sound;

// ============================================================================
// INPUT SYSTEM
// ============================================================================

class InputSystem
{
private:
  Vec2 joystickPos;
  bool firePressed;
  Vec2 touchPos;
  bool isTouching;

  const int JOYSTICK_RADIUS = 60;
  const int JOYSTICK_CENTER_X = 70;
  const int JOYSTICK_CENTER_Y = SCREEN_HEIGHT - 70;

public:
  void update()
  {
    isTouching = false;
    uint16_t tx, ty;

    if (display.getTouch(&tx, &ty))
    {
      isTouching = true;
      touchPos = Vec2(tx, ty);

      // Virtual joystick (left side)
      if (tx < SCREEN_WIDTH / 2)
      {
        float dx = tx - JOYSTICK_CENTER_X;
        float dy = ty - JOYSTICK_CENTER_Y;
        float dist = sqrt(dx * dx + dy * dy);

        if (dist > TOUCH_THRESHOLD)
        {
          float maxDist = JOYSTICK_RADIUS;
          if (dist > maxDist)
          {
            dx = (dx / dist) * maxDist;
            dy = (dy / dist) * maxDist;
          }
          joystickPos = Vec2(dx / maxDist, dy / maxDist);
        }
        else
        {
          joystickPos = Vec2(0, 0);
        }
      }

      // Fire button (right side)
      if (tx > SCREEN_WIDTH / 2)
      {
        firePressed = true;
      }
    }
    else
    {
      joystickPos = Vec2(0, 0);
      firePressed = false;
    }
  }

  Vec2 getMovement() const { return joystickPos; }
  bool isFirePressed() const { return firePressed; }
  bool getTouching() const { return isTouching; }

  void drawUI()
  {
    // Draw joystick base
    canvas.drawCircle(JOYSTICK_CENTER_X, JOYSTICK_CENTER_Y, JOYSTICK_RADIUS, TFT_DARKGREY);
    canvas.fillCircle(JOYSTICK_CENTER_X, JOYSTICK_CENTER_Y, JOYSTICK_RADIUS - 2,
                      canvas.color565(40, 40, 40));

    // Draw joystick stick
    int stickX = JOYSTICK_CENTER_X + joystickPos.x * (JOYSTICK_RADIUS - 20);
    int stickY = JOYSTICK_CENTER_Y + joystickPos.y * (JOYSTICK_RADIUS - 20);
    canvas.fillCircle(stickX, stickY, 20, TFT_WHITE);

    // Draw fire button
    canvas.fillCircle(SCREEN_WIDTH - 60, SCREEN_HEIGHT - 60, 40,
                      firePressed ? TFT_RED : TFT_DARKGREY);
    canvas.setTextColor(TFT_WHITE);
    canvas.setTextDatum(MC_DATUM);
    canvas.drawString("FIRE", SCREEN_WIDTH - 60, SCREEN_HEIGHT - 60);
  }
};

InputSystem input;

// ============================================================================
// ENTITY SYSTEM
// ============================================================================

enum EntityType
{
  PLAYER,
  ENEMY_BASIC,
  ENEMY_FAST,
  ENEMY_TANK,
  BULLET_PLAYER,
  BULLET_ENEMY,
  POWERUP_WEAPON,
  POWERUP_HEALTH,
  EXPLOSION,
  PARTICLE
};

struct Entity
{
  bool active;
  EntityType type;
  Vec2 pos;
  Vec2 vel;
  float width, height;
  int health;
  uint32_t color;
  int animFrame;
  unsigned long lastAnimTime;

  void init(EntityType t, Vec2 p, Vec2 v, float w, float h, int hp, uint32_t col)
  {
    active = true;
    type = t;
    pos = p;
    vel = v;
    width = w;
    height = h;
    health = hp;
    color = col;
    animFrame = 0;
    lastAnimTime = millis();
  }

  Rect getRect() const
  {
    return Rect(pos.x - width / 2, pos.y - height / 2, width, height);
  }

  void deactivate()
  {
    active = false;
  }
};

// ============================================================================
// GAME STATE & ENTITIES
// ============================================================================

class Game
{
public:
  Entity player;
  Entity enemies[MAX_ENEMIES];
  Entity playerBullets[MAX_PLAYER_BULLETS];
  Entity enemyBullets[MAX_ENEMY_BULLETS];
  Entity powerups[MAX_POWERUPS];
  Entity explosions[MAX_EXPLOSIONS];
  Entity particles[MAX_PARTICLES];

  int score;
  int lives;
  int wave;
  float scrollY;
  unsigned long lastEnemySpawn;
  unsigned long lastPlayerShot;
  int playerWeaponLevel;

  enum GameState
  {
    TITLE,
    PLAYING,
    GAME_OVER
  };

  GameState state;

  void init()
  {
    state = TITLE;
    score = 0;
    lives = 3;
    wave = 1;
    scrollY = 0;
    playerWeaponLevel = 1;
    lastEnemySpawn = 0;
    lastPlayerShot = 0;

    // Initialize player
    player.init(PLAYER, Vec2(SCREEN_WIDTH / 2, SCREEN_HEIGHT - 60),
                Vec2(0, 0), 24, 24, 100, TFT_CYAN);

    // Deactivate all entities
    for (int i = 0; i < MAX_ENEMIES; i++)
      enemies[i].active = false;
    for (int i = 0; i < MAX_PLAYER_BULLETS; i++)
      playerBullets[i].active = false;
    for (int i = 0; i < MAX_ENEMY_BULLETS; i++)
      enemyBullets[i].active = false;
    for (int i = 0; i < MAX_POWERUPS; i++)
      powerups[i].active = false;
    for (int i = 0; i < MAX_EXPLOSIONS; i++)
      explosions[i].active = false;
    for (int i = 0; i < MAX_PARTICLES; i++)
      particles[i].active = false;
  }

  void startGame()
  {
    init();
    state = PLAYING;
  }

  // Entity spawning
  void spawnEnemy(EntityType type, Vec2 pos, Vec2 vel)
  {
    for (int i = 0; i < MAX_ENEMIES; i++)
    {
      if (!enemies[i].active)
      {
        int hp = 10;
        uint32_t col = TFT_RED;
        float w = 20, h = 20;

        switch (type)
        {
        case ENEMY_FAST:
          hp = 5;
          col = TFT_YELLOW;
          w = h = 16;
          break;
        case ENEMY_TANK:
          hp = 30;
          col = TFT_PURPLE;
          w = h = 28;
          break;
        default:
          break;
        }

        enemies[i].init(type, pos, vel, w, h, hp, col);
        break;
      }
    }
  }

  void spawnPlayerBullet(Vec2 pos, Vec2 vel)
  {
    for (int i = 0; i < MAX_PLAYER_BULLETS; i++)
    {
      if (!playerBullets[i].active)
      {
        playerBullets[i].init(BULLET_PLAYER, pos, vel, 4, 8, 1, TFT_WHITE);
        break;
      }
    }
  }

  void spawnEnemyBullet(Vec2 pos, Vec2 vel)
  {
    for (int i = 0; i < MAX_ENEMY_BULLETS; i++)
    {
      if (!enemyBullets[i].active)
      {
        enemyBullets[i].init(BULLET_ENEMY, pos, vel, 4, 8, 1, TFT_ORANGE);
        break;
      }
    }
  }

  void spawnExplosion(Vec2 pos, float size)
  {
    for (int i = 0; i < MAX_EXPLOSIONS; i++)
    {
      if (!explosions[i].active)
      {
        explosions[i].init(EXPLOSION, pos, Vec2(0, 0), size, size, 6, TFT_ORANGE);
        break;
      }
    }

    // Spawn particles
    for (int j = 0; j < 8; j++)
    {
      float angle = (j / 8.0) * 2 * PI;
      Vec2 vel(cos(angle) * 2, sin(angle) * 2);
      spawnParticle(pos, vel);
    }
  }

  void spawnParticle(Vec2 pos, Vec2 vel)
  {
    for (int i = 0; i < MAX_PARTICLES; i++)
    {
      if (!particles[i].active)
      {
        particles[i].init(PARTICLE, pos, vel, 2, 2, 10, TFT_YELLOW);
        break;
      }
    }
  }

  void spawnPowerup(Vec2 pos, EntityType type)
  {
    for (int i = 0; i < MAX_POWERUPS; i++)
    {
      if (!powerups[i].active)
      {
        uint32_t col = type == POWERUP_WEAPON ? TFT_GREEN : TFT_MAGENTA;
        powerups[i].init(type, pos, Vec2(0, 1), 16, 16, 1, col);
        break;
      }
    }
  }

  // Update functions
  void update()
  {
    if (state == TITLE)
    {
      if (input.getTouching())
      {
        startGame();
      }
      return;
    }

    if (state == GAME_OVER)
    {
      if (input.getTouching())
      {
        startGame();
      }
      return;
    }

    // Update scroll
    scrollY += 1.0;
    if (scrollY > 32)
      scrollY = 0;

    // Update player
    updatePlayer();

    // Spawn enemies
    if (millis() - lastEnemySpawn > 2000)
    {
      int enemyType = random(0, 100);
      EntityType type = ENEMY_BASIC;
      float speed = 1.5;

      if (enemyType > 70)
      {
        type = ENEMY_FAST;
        speed = 3.0;
      }
      else if (enemyType > 90)
      {
        type = ENEMY_TANK;
        speed = 0.8;
      }

      float x = random(30, SCREEN_WIDTH - 30);
      spawnEnemy(type, Vec2(x, -20), Vec2(0, speed));
      lastEnemySpawn = millis();
    }

    // Update enemies
    updateEnemies();

    // Update bullets
    updateBullets();

    // Update powerups
    updatePowerups();

    // Update explosions
    updateExplosions();

    // Update particles
    updateParticles();

    // Check collisions
    checkCollisions();

    // Check game over
    if (lives <= 0)
    {
      state = GAME_OVER;
    }
  }

  void updatePlayer()
  {
    Vec2 movement = input.getMovement();
    player.vel = movement * 5.0;
    player.pos = player.pos + player.vel;

    // Clamp to screen
    player.pos.x = constrain(player.pos.x, player.width / 2, SCREEN_WIDTH - player.width / 2);
    player.pos.y = constrain(player.pos.y, player.height / 2, SCREEN_HEIGHT - player.height / 2 - 20);

    // Shooting
    if (input.isFirePressed() && millis() - lastPlayerShot > 150)
    {
      sound.play(SoundSystem::SHOOT);

      if (playerWeaponLevel == 1)
      {
        spawnPlayerBullet(player.pos, Vec2(0, -8));
      }
      else if (playerWeaponLevel == 2)
      {
        spawnPlayerBullet(player.pos + Vec2(-8, 0), Vec2(0, -8));
        spawnPlayerBullet(player.pos + Vec2(8, 0), Vec2(0, -8));
      }
      else
      {
        spawnPlayerBullet(player.pos, Vec2(0, -8));
        spawnPlayerBullet(player.pos + Vec2(-8, 0), Vec2(-1, -8));
        spawnPlayerBullet(player.pos + Vec2(8, 0), Vec2(1, -8));
      }

      lastPlayerShot = millis();
    }
  }

  void updateEnemies()
  {
    for (int i = 0; i < MAX_ENEMIES; i++)
    {
      if (!enemies[i].active)
        continue;

      // enemies[i].pos = enemies[i].pos + enemies[i].vel;

      Vec2 dir = (player.pos - enemies[i].pos).normalize();        
      enemies[i].pos.x = enemies[i].pos.x + dir.x * enemies[i].vel.y * 1.5;
      enemies[i].pos.y = enemies[i].pos.y + enemies[i].vel.y;

      // Remove if off screen
      if (enemies[i].pos.y > SCREEN_HEIGHT + 20)
      {
        enemies[i].deactivate();
        continue;
      }

      // Enemy shooting
      if (random(0, 100) < 2)
      {
        // Vec2 dir = (player.pos - enemies[i].pos).normalize();
        // spawnEnemyBullet(enemies[i].pos, dir * 3.0);

        spawnEnemyBullet(enemies[i].pos, Vec2(0, 3));
        sound.play(SoundSystem::ENEMY_SHOOT);
      }
    }
  }

  void updateBullets()
  {
    // Player bullets
    for (int i = 0; i < MAX_PLAYER_BULLETS; i++)
    {
      if (!playerBullets[i].active)
        continue;
      playerBullets[i].pos = playerBullets[i].pos + playerBullets[i].vel;
      if (playerBullets[i].pos.y < -10)
        playerBullets[i].deactivate();
    }

    // Enemy bullets
    for (int i = 0; i < MAX_ENEMY_BULLETS; i++)
    {
      if (!enemyBullets[i].active)
        continue;
      enemyBullets[i].pos = enemyBullets[i].pos + enemyBullets[i].vel;
      if (enemyBullets[i].pos.y > SCREEN_HEIGHT + 10)
        enemyBullets[i].deactivate();
    }
  }

  void updatePowerups()
  {
    for (int i = 0; i < MAX_POWERUPS; i++)
    {
      if (!powerups[i].active)
        continue;
      powerups[i].pos = powerups[i].pos + powerups[i].vel;
      if (powerups[i].pos.y > SCREEN_HEIGHT + 20)
        powerups[i].deactivate();
    }
  }

  void updateExplosions()
  {
    for (int i = 0; i < MAX_EXPLOSIONS; i++)
    {
      if (!explosions[i].active)
        continue;

      if (millis() - explosions[i].lastAnimTime > 50)
      {
        explosions[i].animFrame++;
        explosions[i].lastAnimTime = millis();
        if (explosions[i].animFrame >= explosions[i].health)
        {
          explosions[i].deactivate();
        }
      }
    }
  }

  void updateParticles()
  {
    for (int i = 0; i < MAX_PARTICLES; i++)
    {
      if (!particles[i].active)
        continue;
      particles[i].pos = particles[i].pos + particles[i].vel;
      particles[i].health--;
      if (particles[i].health <= 0)
        particles[i].deactivate();
    }
  }

  void checkCollisions()
  {
    // Player bullets vs enemies
    for (int i = 0; i < MAX_PLAYER_BULLETS; i++)
    {
      if (!playerBullets[i].active)
        continue;

      for (int j = 0; j < MAX_ENEMIES; j++)
      {
        if (!enemies[j].active)
          continue;

        if (playerBullets[i].getRect().intersects(enemies[j].getRect()))
        {
          playerBullets[i].deactivate();
          enemies[j].health -= 10;

          if (enemies[j].health <= 0)
          {
            score += 100;
            spawnExplosion(enemies[j].pos, enemies[j].width);
            sound.play(SoundSystem::EXPLOSION);

            // Chance to drop powerup
            if (random(0, 100) < 20)
            {
              EntityType pType = random(0, 2) == 0 ? POWERUP_WEAPON : POWERUP_HEALTH;
              spawnPowerup(enemies[j].pos, pType);
            }

            enemies[j].deactivate();
          }
          else
          {
            sound.play(SoundSystem::HIT);
          }
          break;
        }
      }
    }

    // Enemy bullets vs player
    for (int i = 0; i < MAX_ENEMY_BULLETS; i++)
    {
      if (!enemyBullets[i].active)
        continue;

      if (enemyBullets[i].getRect().intersects(player.getRect()))
      {
        enemyBullets[i].deactivate();
        lives--;
        spawnExplosion(player.pos, player.width);
        sound.play(SoundSystem::HIT);
      }
    }

    // Enemies vs player
    for (int i = 0; i < MAX_ENEMIES; i++)
    {
      if (!enemies[i].active)
        continue;

      if (enemies[i].getRect().intersects(player.getRect()))
      {
        lives--;
        spawnExplosion(enemies[i].pos, enemies[i].width);
        spawnExplosion(player.pos, player.width);
        sound.play(SoundSystem::EXPLOSION);
        enemies[i].deactivate();
      }
    }

    // Powerups vs player
    for (int i = 0; i < MAX_POWERUPS; i++)
    {
      if (!powerups[i].active)
        continue;

      if (powerups[i].getRect().intersects(player.getRect()))
      {
        if (powerups[i].type == POWERUP_WEAPON)
        {
          playerWeaponLevel = min(playerWeaponLevel + 1, 3);
        }
        else if (powerups[i].type == POWERUP_HEALTH)
        {
          lives = min(lives + 1, 5);
        }
        sound.play(SoundSystem::POWERUP);
        powerups[i].deactivate();
      }
    }
  }

  // Rendering
  void render()
  {
    canvas.fillSprite(TFT_BLACK);

    if (state == TITLE)
    {
      renderTitle();
    }
    else if (state == PLAYING)
    {
      renderGame();
    }
    else if (state == GAME_OVER)
    {
      renderGameOver();
    }

    canvas.pushSprite(0, 0);
  }

  void renderTitle()
  {
    canvas.setTextColor(TFT_CYAN);
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextSize(3);
    canvas.drawString("SPACE STRIKER", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 - 40);

    canvas.setTextSize(2);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("Touch to Start", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 + 20);

    canvas.setTextSize(1);
    canvas.setTextColor(TFT_YELLOW);
    canvas.drawString("90s Arcade Style", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 + 60);
  }

  void renderGameOver()
  {
    canvas.setTextColor(TFT_RED);
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextSize(3);
    canvas.drawString("GAME OVER", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 - 40);

    canvas.setTextSize(2);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("Score: " + String(score), SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 + 20);

    canvas.setTextSize(1);
    canvas.setTextColor(TFT_YELLOW);
    canvas.drawString("Touch to Restart", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 + 60);
  }

  void renderGame()
  {
    // Draw scrolling background
    drawBackground();

    // Draw entities
    drawParticles();
    drawPowerups();
    drawBullets();
    drawEnemies();
    drawPlayer();
    drawExplosions();

    // Draw UI
    drawHUD();
    input.drawUI();
  }

  void drawBackground()
  {
    // Simple star field
    for (int y = -32; y < SCREEN_HEIGHT; y += 32)
    {
      for (int x = 0; x < SCREEN_WIDTH; x += 40)
      {
        int starY = (int)(y + scrollY) % SCREEN_HEIGHT;
        canvas.fillCircle(x + (y / 32) * 20, starY, 1, TFT_DARKGREY);
      }
    }
  }

  void drawPlayer()
  {
    // Simple triangle ship (placeholder for your pixel art)
    // canvas.fillTriangle(
    //   player.pos.x, player.pos.y - player.height/2,
    //   player.pos.x - player.width/2, player.pos.y + player.height/2,
    //   player.pos.x + player.width/2, player.pos.y + player.height/2,
    //   player.color
    // );
    // canvas.drawTriangle(
    //   player.pos.x, player.pos.y - player.height/2,
    //   player.pos.x - player.width/2, player.pos.y + player.height/2,
    //   player.pos.x + player.width/2, player.pos.y + player.height/2,
    //   TFT_WHITE
    // );

    int x = player.pos.x - player.width / 2;
    int y = player.pos.y - player.height / 2;
    canvas.pushImage(x, y, 24, 24, player_ship_map);
  }

  void drawEnemies()
  {
    for (int i = 0; i < MAX_ENEMIES; i++)
    {
      if (!enemies[i].active)
        continue;

      int x = enemies[i].pos.x - enemies[i].width / 2;
      int y = enemies[i].pos.y - enemies[i].height / 2;

      // Choose sprite based on enemy type
      const uint16_t *sprite;
      int w, h;

      switch (enemies[i].type)
      {
      case ENEMY_BASIC:
        sprite = enemy_basic_map;
        w = h = 20;
        break;
      case ENEMY_FAST:
        sprite = enemy_fast_map;
        w = h = 16;
        break;
      case ENEMY_TANK:
        sprite = enemy_tank_map;
        w = h = 28;
        break;
      }

      canvas.pushImage(x, y, w, h, sprite);
    }
  }

  void drawBullets()
  {
    // Player bullets
    for (int i = 0; i < MAX_PLAYER_BULLETS; i++)
    {
      if (!playerBullets[i].active)
        continue;
      int x = playerBullets[i].pos.x - 2;
      int y = playerBullets[i].pos.y - 4;
      canvas.pushImage(x, y, 4, 8, bullet_player_map);
    }

    // Enemy bullets
    for (int i = 0; i < MAX_ENEMY_BULLETS; i++)
    {
      if (!enemyBullets[i].active)
        continue;
      int x = enemyBullets[i].pos.x - 2;
      int y = enemyBullets[i].pos.y - 4;
      canvas.pushImage(x, y, 4, 8, bullet_enemy_map);
    }
  }

  void drawPowerups()
  {
    for (int i = 0; i < MAX_POWERUPS; i++)
    {
      if (!powerups[i].active)
        continue;

      int x = powerups[i].pos.x - powerups[i].width / 2;
      int y = powerups[i].pos.y - powerups[i].height / 2;

      const uint16_t *sprite = (powerups[i].type == POWERUP_WEAPON)
                                   ? powerup_weapon_map
                                   : powerup_health_map;

      canvas.pushImage(x, y, 16, 16, sprite);
    }
  }

  void drawExplosions()
  {
    for (int i = 0; i < MAX_EXPLOSIONS; i++)
    {
      if (!explosions[i].active)
        continue;

      int frame = explosions[i].animFrame;
      float scale = 1.0 + (frame * 0.3);
      int size = explosions[i].width * scale;

      // Expanding circles
      canvas.drawCircle(explosions[i].pos.x, explosions[i].pos.y,
                        size / 2, TFT_ORANGE);
      canvas.drawCircle(explosions[i].pos.x, explosions[i].pos.y,
                        size / 3, TFT_YELLOW);
    }
  }

  void drawParticles()
  {
    for (int i = 0; i < MAX_PARTICLES; i++)
    {
      if (!particles[i].active)
        continue;
      canvas.fillCircle(particles[i].pos.x, particles[i].pos.y, 2, particles[i].color);
    }
  }

  void drawHUD()
  {
    canvas.setTextColor(TFT_WHITE);
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(2);

    // Score
    canvas.drawString("SCORE: " + String(score), 10, 10);

    // Lives
    canvas.drawString("LIVES:", 10, 40);
    for (int i = 0; i < lives; i++)
    {
      canvas.fillTriangle(
          100 + i * 25, 40,
          95 + i * 25, 50,
          105 + i * 25, 50,
          TFT_CYAN);
    }

    // Weapon level
    canvas.drawString("WPN: " + String(playerWeaponLevel), 10, 70);
  }
};
Game game;

// ============================================================================
// ARDUINO SETUP & LOOP
// ============================================================================

void setup()
{

  Serial.begin(115200);
  Serial.println("Space Striker Starting...");

  // Backlight
  pinMode(46, OUTPUT);
  digitalWrite(46, HIGH);

  // Initialize display
  display.init();
  display.setRotation(1);
  display.fillScreen(TFT_BLACK);

  // Create sprite for double buffering
  canvas.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT);
  canvas.setColorDepth(16);

  // Initialize systems
  sound.init();
  game.init();

  Serial.println("Game initialized!");
}

void loop()
{
  static unsigned long lastFrame = 0;
  unsigned long currentTime = millis();

  if (currentTime - lastFrame >= FRAME_TIME)
  {
    // Update input
    input.update();

    // Update game
    game.update();

    // Update sound
    sound.update();

    // Render
    game.render();

    lastFrame = currentTime;

    // Debug FPS
    static unsigned long lastFpsUpdate = 0;
    static int frameCount = 0;
    frameCount++;
    if (currentTime - lastFpsUpdate > 1000)
    {
      Serial.print("FPS: ");
      Serial.println(frameCount);
      frameCount = 0;
      lastFpsUpdate = currentTime;
    }
  }
}

// ============================================================================
// NOTES FOR EXTENDING THE GAME
// ============================================================================

/*
 * HOW TO ADD YOUR OWN PIXEL ART SPRITES:
 *
 * 1. Create sprites as RGB565 arrays:
 *    - Use tools like LVGL Image Converter or custom scripts
 *    - Store in PROGMEM to save RAM
 *
 *    Example:
 *    const uint16_t player_sprite[] PROGMEM = {
 *      0x001F, 0x001F, 0x001F, ...
 *    };
 *
 * 2. Replace drawing functions:
 *    - In drawPlayer(), replace fillTriangle with:
 *      canvas.pushImage(x, y, width, height, player_sprite);
 *
 * 3. Add sprite sheets for animations:
 *    - Store multiple frames
 *    - Update animFrame in entity
 *    - Draw correct frame based on animFrame
 *
 * EXTENDING ENEMY TYPES:
 *
 * 1. Add new EntityType enum value
 * 2. Add case in spawnEnemy() with unique properties
 * 3. Add custom behavior in updateEnemies():
 *    - Sine wave movement
 *    - Circular patterns
 *    - Formation flying
 *
 * ADDING NEW WEAPONS:
 *
 * 1. Increase playerWeaponLevel range
 * 2. Add new bullet patterns in updatePlayer()
 * 3. Examples:
 *    - Spread shot
 *    - Homing missiles
 *    - Laser beams
 *
 * ADDING BOSS BATTLES:
 *
 * 1. Create Boss entity with high HP
 * 2. Add boss-specific update function
 * 3. Implement attack patterns:
 *    - Multiple bullet spreads
 *    - Spawn minions
 *    - Move in patterns
 *
 * ADDING PARALLAX BACKGROUNDS:
 *
 * 1. Create multiple background layers
 * 2. Scroll at different speeds
 * 3. Use sprites for tiles
 *
 * SAVE/LOAD HIGH SCORES:
 *
 * 1. Use Preferences library
 * 2. Save on game over
 * 3. Display on title screen
 *
 * OPTIMIZATIONS:
 *
 * 1. Use spatial hashing for collision detection if too slow
 * 2. Reduce entity counts if frame rate drops
 * 3. Use dirty rectangles for partial screen updates
 * 4. Pre-render static backgrounds to sprite
 */