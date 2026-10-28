// ===================== C5 (ESP32-C5) =====================
#include <Arduino.h>
#include <WiFi.h>
#include "esp_heap_caps.h"

extern "C" {
  #include "lwip/sockets.h"
  #include "lwip/netdb.h"
  #include "driver/spi_slave.h"
  #include "driver/gpio.h"
  #include <esp_wifi.h>
}

// ---------------- CONFIG ----------------
static const char* WIFI_SSID = "MKUH-Wi-Fi";
static const char* WIFI_PASS = "";

static const char* HOST = "uk3.internet-radio.com";
static const int   PORT = 8347;
static const char* PATH = "/stream?type=.mp3";

// ---------------- PINS ------------------
static constexpr gpio_num_t PIN_SPI_SCLK = GPIO_NUM_1;
static constexpr gpio_num_t PIN_SPI_MOSI = GPIO_NUM_6;
static constexpr gpio_num_t PIN_SPI_MISO = GPIO_NUM_0;
static constexpr gpio_num_t PIN_SPI_CS   = GPIO_NUM_24;
static constexpr gpio_num_t PIN_READY    = GPIO_NUM_23;

// --------------- PROTOCOL ---------------
static constexpr uint8_t  MAGIC0 = 0xA5;
static constexpr uint8_t  MAGIC1 = 0x5A;
static constexpr uint16_t CHUNK_PAYLOAD = 1024;

enum : uint8_t { TYPE_AUDIO = 0x01 };
enum : uint8_t { FLAG_WIFI_CONNECTED = 1 << 0, FLAG_C5_AWAKE = 1 << 1 };

// CRC16-CCITT
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
  uint16_t crc_le;
};

struct __attribute__((packed)) Frame {
  ChunkHeader hdr;
  uint8_t payload[CHUNK_PAYLOAD];
};

// ------------- AUDIO CHUNK QUEUE (HTTP -> SPI) -------------
struct AudioChunk {
  uint16_t len;
  uint8_t  data[CHUNK_PAYLOAD];
};

static constexpr int QDEPTH = 32;
static AudioChunk q[QDEPTH];
static volatile int q_head = 0;
static volatile int q_tail = 0;

static portMUX_TYPE qMux = portMUX_INITIALIZER_UNLOCKED;

static inline int q_next(int i) { return (i + 1) % QDEPTH; }

static inline bool q_empty_locked() { return q_head == q_tail; }
static inline bool q_full_locked()  { return q_next(q_head) == q_tail; }

static inline void ready_set(bool hi) {
  gpio_set_level(PIN_READY, hi ? 1 : 0);
}

static void ready_update_from_state() {
  portENTER_CRITICAL(&qMux);
  bool empty = q_empty_locked();
  portEXIT_CRITICAL(&qMux);
  ready_set(!empty);
}

static bool q_push_audio(const uint8_t* data, uint16_t len) {
  if (len > CHUNK_PAYLOAD) len = CHUNK_PAYLOAD;

  portENTER_CRITICAL(&qMux);
  if (q_full_locked()) {
    portEXIT_CRITICAL(&qMux);
    return false;
  }
  AudioChunk& c = q[q_head];
  c.len = len;
  memcpy(c.data, data, len);
  q_head = q_next(q_head);
  portEXIT_CRITICAL(&qMux);

  // Hint to master
  ready_set(true);
  return true;
}

static bool q_pop_chunk(uint8_t* out, uint16_t* out_len) {
  portENTER_CRITICAL(&qMux);
  if (q_empty_locked()) {
    portEXIT_CRITICAL(&qMux);
    return false;
  }
  AudioChunk& c = q[q_tail];
  uint16_t len = c.len;
  memcpy(out, c.data, len);
  q_tail = q_next(q_tail);
  bool empty_now = q_empty_locked();
  portEXIT_CRITICAL(&qMux);

  *out_len = len;
  if (empty_now) ready_set(false);
  return true;
}

// --------------- FRAME FILL (SPI owns DMA Frames) ---------------
static uint8_t seqno = 0;

static void fill_frame(Frame& f, const uint8_t* data, uint16_t len) {
  if (len > CHUNK_PAYLOAD) len = CHUNK_PAYLOAD;

  uint8_t flags = 0;
  if (WiFi.status() == WL_CONNECTED) flags |= FLAG_WIFI_CONNECTED;
  flags |= FLAG_C5_AWAKE;

  f.hdr.m0 = MAGIC0;
  f.hdr.m1 = MAGIC1;
  f.hdr.type = TYPE_AUDIO;
  f.hdr.flags = flags;
  f.hdr.len_le = len;
  f.hdr.seq = seqno++;

  if (len) memcpy(f.payload, data, len);
  if (len < CHUNK_PAYLOAD) memset(f.payload + len, 0, CHUNK_PAYLOAD - len);

  uint16_t crc = 0xFFFF;
  crc = crc16_ccitt((const uint8_t*)&f.hdr.type, 1 + 1 + 2 + 1, crc);
  crc = crc16_ccitt(f.payload, len, crc);
  f.hdr.crc_le = crc;
}

// ---------------- SPI SLAVE (ALWAYS QUEUED) ----------------
static constexpr int SLAVE_Q = 3;

static Frame* dmaFrames[SLAVE_Q];
static spi_slave_transaction_t trans[SLAVE_Q];

// temp buffer used to copy a chunk out of the queue
static uint8_t tmp_payload[CHUNK_PAYLOAD];

static void spi_slave_init_bus() {
  // READY GPIO
  gpio_config_t io = {};
  io.pin_bit_mask = 1ULL << PIN_READY;
  io.mode = GPIO_MODE_OUTPUT;
  gpio_config(&io);
  ready_set(false);

  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = PIN_SPI_MOSI;
  buscfg.miso_io_num = PIN_SPI_MISO;
  buscfg.sclk_io_num = PIN_SPI_SCLK;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;

  spi_slave_interface_config_t slvcfg = {};
  slvcfg.spics_io_num = PIN_SPI_CS;
  slvcfg.queue_size = SLAVE_Q;
  slvcfg.mode = 0;

  esp_err_t err = spi_slave_initialize(SPI2_HOST, &buscfg, &slvcfg, SPI_DMA_CH_AUTO);
  if (err != ESP_OK) {
    Serial.printf("spi_slave_initialize failed: %d\n", (int)err);
    while (true) delay(1000);
  }
}

static void spi_task(void*) {
  // Allocate DMA-capable frames
  for (int i = 0; i < SLAVE_Q; i++) {
    dmaFrames[i] = (Frame*)heap_caps_malloc(sizeof(Frame), MALLOC_CAP_DMA);
    if (!dmaFrames[i]) {
      Serial.println("heap_caps_malloc(MALLOC_CAP_DMA) failed");
      while (true) delay(1000);
    }
    memset(dmaFrames[i], 0, sizeof(Frame));
    memset(&trans[i], 0, sizeof(trans[i]));
    trans[i].length = sizeof(Frame) * 8;
    trans[i].tx_buffer = dmaFrames[i];
  }

  // Prime the queue with SLAVE_Q transactions (empty frames until audio arrives)
  for (int i = 0; i < SLAVE_Q; i++) {
    uint16_t len = 0;
    if (q_pop_chunk(tmp_payload, &len)) {
      // queue not empty: READY already set
    } else {
      len = 0;
      // keep READY accurate
      ready_update_from_state();
    }
    fill_frame(*dmaFrames[i], tmp_payload, len);

    esp_err_t e = spi_slave_queue_trans(SPI2_HOST, &trans[i], portMAX_DELAY);
    if (e != ESP_OK) Serial.printf("queue_trans err=%d\n", (int)e);
  }

  // Recycle frames forever: always keep the slave armed
  for (;;) {
    spi_slave_transaction_t* done = nullptr;
    esp_err_t e = spi_slave_get_trans_result(SPI2_HOST, &done, portMAX_DELAY);
    if (e != ESP_OK || !done) continue;

    Frame* f = (Frame*)done->tx_buffer;

    uint16_t len = 0;
    if (!q_pop_chunk(tmp_payload, &len)) {
      len = 0;
      ready_update_from_state();
    }
    fill_frame(*f, tmp_payload, len);

    e = spi_slave_queue_trans(SPI2_HOST, done, portMAX_DELAY);
    if (e != ESP_OK) Serial.printf("requeue err=%d\n", (int)e);
  }
}

// ---------------- HTTP streamer ----------------
static int connect_tcp(const char* host, int port) {
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;

  char portstr[8];
  snprintf(portstr, sizeof(portstr), "%d", port);

  if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return -1;

  int s = socket(res->ai_family, res->ai_socktype, 0);
  if (s < 0) { freeaddrinfo(res); return -1; }

  if (connect(s, res->ai_addr, res->ai_addrlen) != 0) {
    close(s);
    freeaddrinfo(res);
    return -1;
  }

  freeaddrinfo(res);
  return s;
}

static bool read_until_headers_done(int s) {
  const char pat[4] = {'\r','\n','\r','\n'};
  int matched = 0;
  uint8_t b;

  while (true) {
    int r = recv(s, &b, 1, 0);
    if (r <= 0) return false;
    if (b == (uint8_t)pat[matched]) {
      matched++;
      if (matched == 4) return true;
    } else {
      matched = (b == (uint8_t)pat[0]) ? 1 : 0;
    }
  }
}

static void http_task(void*) {
  uint8_t buf[CHUNK_PAYLOAD];

  while (true) {
    if (WiFi.status() != WL_CONNECTED) {
      delay(500);
      continue;
    }

    int s = connect_tcp(HOST, PORT);
    if (s < 0) {
      Serial.println("TCP connect failed");
      delay(1000);
      continue;
    }

    String req =
      String("GET ") + PATH + " HTTP/1.1\r\n" +
      "Host: " + HOST + ":" + String(PORT) + "\r\n" +
      "User-Agent: MWR3.5\r\n" +
      "Connection: close\r\n" +
      "Icy-MetaData: 0\r\n\r\n";

    send(s, req.c_str(), req.length(), 0);

    if (!read_until_headers_done(s)) {
      Serial.println("Header read failed");
      close(s);
      delay(500);
      continue;
    }

    Serial.println("Streaming...");

    while (true) {
      int r = recv(s, buf, sizeof(buf), 0);
      if (r <= 0) break;

      int off = 0;
      while (off < r) {
        int n = r - off;
        if (n > (int)CHUNK_PAYLOAD) n = CHUNK_PAYLOAD;

        if (q_push_audio(buf + off, (uint16_t)n)) {
          off += n;
        } else {
          // Queue full
          delay(2);
        }
      }
    }

    Serial.println("Stream ended, reconnecting...");
    close(s);
    delay(500);
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println("C5 starting WiFi...");
  WiFi.mode(WIFI_STA);

  esp_err_t err = esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY);
  Serial.printf("esp_wifi_set_band_mode(5G) -> %d\n", (int)err);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  spi_slave_init_bus();
  ready_update_from_state();

  xTaskCreatePinnedToCore(spi_task,  "spi_task",  4096, nullptr, 3, nullptr, 0);
  xTaskCreatePinnedToCore(http_task, "http_task", 6144, nullptr, 2, nullptr, 0);
}

void loop() { delay(1000); }
