// ===================== S3 (ESP32-S3) =====================
#include <Arduino.h>
#include <SPI.h>
#include <Client.h>
#include "Audio.h"   // AllanGallop/ESP32-audioI2S fork

// -------------------- Pins (YOUR PCB FIXED) --------------------
// I2S
static constexpr int PIN_I2S_BCLK = 41;
static constexpr int PIN_I2S_LRC  = 39;
static constexpr int PIN_I2S_DOUT = 40;

// Audio power MOSFET (you can power C5 from this too)
static constexpr int PIN_AUDIO_PWR = 38;

// LEDs
static constexpr int PIN_LED_RED   = 13;
static constexpr int PIN_LED_AMBER = 14;
static constexpr int PIN_LED_GREEN = 21;
static constexpr int PIN_LED_WIFI  = 47; // blue

// Encoder + button
static constexpr int PIN_ENC_A   = 17;
static constexpr int PIN_ENC_B   = 16;
static constexpr int PIN_ENC_BTN = 8;

// Battery / charging
static constexpr int PIN_CHG_5V_SENSE = 4; // 5V present
static constexpr int PIN_VBAT_ADC     = 5; // divider output

// -------------------- SPI link pins (S3 SAFE SET) --------------------
static constexpr int PIN_SPI_SCLK  = 11;
static constexpr int PIN_SPI_MOSI  = 10;
static constexpr int PIN_SPI_MISO  = 12;
static constexpr int PIN_SPI_CS    = 9;
static constexpr int PIN_SPI_READY = 3;

static constexpr uint32_t SPI_HZ = 4000000; // raise later if wiring is solid

// -------------------- Protocol --------------------
static constexpr uint8_t  MAGIC0 = 0xA5;
static constexpr uint8_t  MAGIC1 = 0x5A;
static constexpr uint16_t CHUNK_PAYLOAD = 1024;

enum : uint8_t { TYPE_AUDIO = 0x01 };
enum : uint8_t { FLAG_WIFI_CONNECTED = 1 << 0, FLAG_C5_AWAKE = 1 << 1 };

// CRC16-CCITT (0x1021), init 0xFFFF
static uint16_t crc16_ccitt(const uint8_t* data, size_t len, uint16_t crc = 0xFFFF) {
  while (len--) {
    crc ^= (uint16_t)(*data++) << 8;
    for (int i = 0; i < 8; i++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

struct __attribute__((packed)) ChunkHeader {
  uint8_t  m0, m1;
  uint8_t  type;
  uint8_t  flags;
  uint16_t len_le;
  uint8_t  seq;
  uint16_t crc_le; // CRC over (type..seq + payload[0..len-1])
};

struct __attribute__((packed)) Frame {
  ChunkHeader hdr;
  uint8_t payload[CHUNK_PAYLOAD];
};

// -------------------- Ring buffer (MP3 bytes from C5) --------------------
static constexpr size_t RB_SIZE = 128 * 1024;
static uint8_t rb[RB_SIZE];
static size_t rb_head = 0;
static size_t rb_tail = 0;
static portMUX_TYPE rbMux = portMUX_INITIALIZER_UNLOCKED;

static inline size_t rb_used_locked() {
  return (rb_head >= rb_tail) ? (rb_head - rb_tail) : (RB_SIZE - (rb_tail - rb_head));
}
static inline size_t rb_free_locked() {
  return RB_SIZE - 1 - rb_used_locked();
}

static size_t rb_used() {
  portENTER_CRITICAL(&rbMux);
  size_t u = rb_used_locked();
  portEXIT_CRITICAL(&rbMux);
  return u;
}
static size_t rb_free() {
  portENTER_CRITICAL(&rbMux);
  size_t f = rb_free_locked();
  portEXIT_CRITICAL(&rbMux);
  return f;
}

static size_t rb_write(const uint8_t* data, size_t n) {
  portENTER_CRITICAL(&rbMux);
  size_t free = rb_free_locked();
  if (n > free) n = free;

  size_t h = rb_head;
  for (size_t i = 0; i < n; i++) {
    rb[h] = data[i];
    h = (h + 1) % RB_SIZE;
  }
  rb_head = h;
  portEXIT_CRITICAL(&rbMux);
  return n;
}

static size_t rb_read(uint8_t* out, size_t n) {
  portENTER_CRITICAL(&rbMux);
  size_t used = rb_used_locked();
  if (n > used) n = used;

  size_t t = rb_tail;
  for (size_t i = 0; i < n; i++) {
    out[i] = rb[t];
    t = (t + 1) % RB_SIZE;
  }
  rb_tail = t;
  portEXIT_CRITICAL(&rbMux);
  return n;
}

// -------------------- Ring "Client" (feeds AudioI2S) --------------------
static volatile uint32_t rb_underflows = 0;
static volatile bool g_paused = false;


class RingClient : public Client {
public:
  int available() override { return (int)rb_used(); }

  int peek() override {
    if (rb_used() == 0) return -1;
    portENTER_CRITICAL(&rbMux);
    uint8_t b = rb[rb_tail];
    portEXIT_CRITICAL(&rbMux);
    return b;
  }

  int read() override {
    uint8_t b;
    int n = read(&b, 1);
    return (n == 1) ? b : -1;
  }

  int read(uint8_t* buf, size_t size) override {
    if (!buf || size == 0) return 0;

    // If paused, behave like "no data"
    if (g_paused) return 0;

    // Wait up to ~60ms for data
    const uint32_t start = millis();
    while (rb_used() == 0) {
      if (g_paused) return 0;
      if ((millis() - start) > 60) {
        rb_underflows++;
        return 0;
      }
      vTaskDelay(1);
    }

    int n = (int)rb_read(buf, size);
    if (n <= 0) rb_underflows++;
    return n;
  }

  void flush() override {}

  size_t write(uint8_t) override { return 0; }
  size_t write(const uint8_t*, size_t) override { return 0; }

  int connect(IPAddress, uint16_t) override { return 0; }
  int connect(const char*, uint16_t) override { return 0; }
  uint8_t connected() override { return 1; }
  void stop() override {}

  operator bool() override { return true; }
};

static RingClient ringClient;

// -------------------- SPI reader task --------------------
static SPIClass spi(FSPI);
static uint8_t tx_zeros[sizeof(Frame)];

static void spi_read_frame(Frame* f) {
  spi.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_SPI_CS, LOW);
  spi.transferBytes(tx_zeros, (uint8_t*)f, sizeof(Frame));
  digitalWrite(PIN_SPI_CS, HIGH);
  spi.endTransaction();
}

// Stats
static uint32_t spi_ok=0, spi_bad_magic=0, spi_bad_crc=0, spi_bad_type=0, spi_seq_gaps=0;
static uint32_t spi_drops_rb_full=0;
static int last_seq = -1;
static uint8_t last_flags = 0;

static void task_spi_reader(void*) {
  static Frame f;

  for (;;) {
    // Wait until at least one frame queued
    while (digitalRead(PIN_SPI_READY) == LOW) {
      vTaskDelay(1);
    }

    // Drain while READY remains high
    while (digitalRead(PIN_SPI_READY) == HIGH) {
      spi_read_frame(&f);

      const ChunkHeader& h = f.hdr;
      if (h.m0 != MAGIC0 || h.m1 != MAGIC1) { spi_bad_magic++; continue; }
      if (h.type != TYPE_AUDIO)            { spi_bad_type++;  continue; }

      if (last_seq >= 0) {
        uint8_t expect = (uint8_t)((last_seq + 1) & 0xFF);
        if (h.seq != expect) spi_seq_gaps++;
      }
      last_seq = h.seq;
      last_flags = h.flags;

      uint16_t len = h.len_le;
      if (len > CHUNK_PAYLOAD) len = CHUNK_PAYLOAD;

      uint16_t want = h.crc_le;
      uint16_t got  = 0xFFFF;
      got = crc16_ccitt((const uint8_t*)&h.type, 1 + 1 + 2 + 1, got);
      got = crc16_ccitt(f.payload, len, got);
      if (got != want) { spi_bad_crc++; continue; }

      if (len) {
        if (rb_free() < len) spi_drops_rb_full++;
        else rb_write(f.payload, len);
      }

      spi_ok++;
    }
  }
}

// -------------------- Encoder + button --------------------
static volatile int16_t enc_delta = 0;

void IRAM_ATTR isr_encoder() {
  static uint8_t old = 0;
  uint8_t a = (uint8_t)digitalRead(PIN_ENC_A);
  uint8_t b = (uint8_t)digitalRead(PIN_ENC_B);
  uint8_t now = (a << 1) | b;

  static const int8_t tbl[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
  };
  uint8_t idx = (old << 2) | now;
  enc_delta += tbl[idx];
  old = now;
}

static int volume = 10; // 0..21
static uint32_t btn_down_ms = 0;
static constexpr uint32_t LONG_PRESS_MS = 1200;

// -------------------- Battery / LEDs --------------------
static constexpr float VBAT_DIV_RATIO = 2.0f;
static float VBAT_CAL = 1.00f;

static float read_vbat_volts() {
  uint32_t acc = 0;
  const int samples = 16;
  for (int i = 0; i < samples; i++) {
    acc += analogRead(PIN_VBAT_ADC);
    delayMicroseconds(200);
  }
  float raw = (float)acc / (float)samples;
  float v_adc = (raw / 4095.0f) * 3.3f;
  return v_adc * VBAT_DIV_RATIO * VBAT_CAL;
}

static bool is_charger_present() {
  return digitalRead(PIN_CHG_5V_SENSE) == HIGH;
}

static void set_batt_leds(float vbat, bool charging) {
  bool red=false, amber=false, green=false;

  if (vbat >= 4.00f)      { green=true; }
  else if (vbat >= 3.70f) { amber=true; }
  else if (vbat >= 3.40f) { red=true; amber=true; }
  else                    { red=true; }

  static uint32_t lastBlink = 0;
  static bool blinkState = false;
  if (charging) {
    if (millis() - lastBlink > 500) {
      lastBlink = millis();
      blinkState = !blinkState;
    }
    amber = blinkState;
  }

  digitalWrite(PIN_LED_RED,   red   ? HIGH : LOW);
  digitalWrite(PIN_LED_AMBER, amber ? HIGH : LOW);
  digitalWrite(PIN_LED_GREEN, green ? HIGH : LOW);
}

static void power_off_to_sleep() {
  // Pause client reads so Audio won't interpret shutdown weirdly
  g_paused = true;

  digitalWrite(PIN_LED_WIFI, LOW);
  digitalWrite(PIN_LED_RED, LOW);
  digitalWrite(PIN_LED_AMBER, LOW);
  digitalWrite(PIN_LED_GREEN, LOW);

  // Cut power to audio rail
  digitalWrite(PIN_AUDIO_PWR, LOW);

  uint32_t t0 = millis();
  while (digitalRead(PIN_ENC_BTN) == LOW) {
    delay(10);
    if (millis() - t0 > 3000) {
      // safety timeout in case button is stuck
      break;
    }
  }

  // Small debounce delay after release
  delay(50);

  // Optional but recommended: make RTC pull-up explicit for sleep wake
  gpio_pullup_en((gpio_num_t)PIN_ENC_BTN);
  gpio_pulldown_dis((gpio_num_t)PIN_ENC_BTN);

  // Wake when button is pressed again (goes LOW)
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_ENC_BTN, 1);

  esp_deep_sleep_start();
}

// -------------------- AudioI2S --------------------
static Audio audio;
static bool audio_started = false;

// Prebuffer thresholds
static constexpr size_t PREBUFFER_START = 24 * 1024;  // wait for 24kB before starting decode
static constexpr size_t PREBUFFER_RESTART = 16 * 1024;

static void audio_start_if_ready() {
  if (audio_started) return;
  if (rb_used() < PREBUFFER_START) return;

  Serial.printf("Audio START (rb=%u)\n", (unsigned)rb_used());
  g_paused = false;

  audio_started = true;
}

static void set_paused(bool p) {
  g_paused = p;
  digitalWrite(PIN_LED_WIFI, (!p && (last_flags & FLAG_WIFI_CONNECTED)) ? HIGH : LOW);
}

// -------------------- Setup/Loop --------------------
void setup() {
  Serial.begin(115200);
  delay(100);

  memset(tx_zeros, 0, sizeof(tx_zeros));

  // Power + LEDs
  pinMode(PIN_AUDIO_PWR, OUTPUT);
  digitalWrite(PIN_AUDIO_PWR, LOW);

  pinMode(PIN_LED_WIFI, OUTPUT);
  digitalWrite(PIN_LED_WIFI, LOW);

  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_AMBER, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);

  // Charger sense
  pinMode(PIN_CHG_5V_SENSE, INPUT);

  // ADC
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_VBAT_ADC, ADC_11db);

  // Encoder + button
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_BTN, INPUT_PULLUP);
  attachInterrupt(PIN_ENC_A, isr_encoder, CHANGE);
  attachInterrupt(PIN_ENC_B, isr_encoder, CHANGE);

  // SPI
  pinMode(PIN_SPI_CS, OUTPUT);
  digitalWrite(PIN_SPI_CS, HIGH);
  pinMode(PIN_SPI_READY, INPUT_PULLDOWN);
  spi.begin(PIN_SPI_SCLK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SPI_CS);

  // Start SPI reader on core 1
  xTaskCreatePinnedToCore(task_spi_reader, "spi_reader", 4096, nullptr, 3, nullptr, 1);

  // Battery indication
  float vbat = read_vbat_volts();
  bool chg = is_charger_present();
  set_batt_leds(vbat, chg);

  if (!chg && vbat < 3.20f) {
    Serial.printf("VBAT low (%.2fV). Sleeping.\n", vbat);
    power_off_to_sleep();
  }

  // Power up audio rail
  digitalWrite(PIN_AUDIO_PWR, HIGH);
  delay(50);

  // Start AudioI2S
  audio.setPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
  audio.connecttoClient(ringClient, 2 /* MP3 */);
  audio.setVolume((uint8_t)volume);

  // Start paused until prebuffer fills
  set_paused(true);
}

void loop() {
  // Battery housekeeping
  static uint32_t lastBatt = 0;
  if (millis() - lastBatt > 2000) {
    lastBatt = millis();
    float vbat = read_vbat_volts();
    bool chg = is_charger_present();
    set_batt_leds(vbat, chg);

    if (!chg && vbat < 3.10f) {
      Serial.printf("VBAT critical (%.2fV). Sleeping.\n", vbat);
      power_off_to_sleep();
    }
  }

  // Button short press = pause/resume, long press = sleep
  bool down = (digitalRead(PIN_ENC_BTN) == LOW);
  if (down && btn_down_ms == 0) {
    btn_down_ms = millis();
  } else if (!down && btn_down_ms != 0) {
    uint32_t dt = millis() - btn_down_ms;
    btn_down_ms = 0;

    if (dt >= LONG_PRESS_MS) {
      power_off_to_sleep();
    } else if (dt >= 40) {
      set_paused(!g_paused);
    }
  } else if (down && btn_down_ms != 0) {
    if ((millis() - btn_down_ms) >= LONG_PRESS_MS) {
      btn_down_ms = 0;
      power_off_to_sleep();
    }
  }

  // Encoder volume
  int16_t d;
  noInterrupts();
  d = enc_delta;
  enc_delta = 0;
  interrupts();

  if (d != 0) {
    volume += (d > 0) ? 1 : -1;
    if (volume < 0) volume = 0;
    if (volume > 21) volume = 21;
    audio.setVolume((uint8_t)volume);
  }

  // If not paused, enable decode once we have a buffer
  if (!g_paused) {
    audio_start_if_ready();
  } else {
    audio_started = false; // allow restart when unpaused + prebuffer again
  }

  // Wifi LED
  bool wifi = (last_flags & FLAG_WIFI_CONNECTED) != 0;
  digitalWrite(PIN_LED_WIFI, (wifi && !g_paused) ? HIGH : LOW);

  // Keep decoder fed
  audio.loop();

  delay(1);
}
