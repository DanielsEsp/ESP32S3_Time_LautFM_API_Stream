


//       Esp32 DevKit
//       PARTITION:   HUGE APP (3M APP - no OTA - 1M SPIFFS)


#include <Arduino.h>
#include "BluetoothA2DPSource.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

#ifndef ESP_ARDUINO_VERSION_MAJOR
  #define ESP_ARDUINO_VERSION_MAJOR 2
#endif
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #include "driver/i2s_std.h"          // core 3.x : nouveau pilote I2S
#else
  #include "driver/i2s.h"              // core 2.x : pilote I2S classique
#endif

// ===================== À ADAPTER =====================
const char* BT_NAME = "JBL Clip+";   // nom EXACT vu dans la liste Bluetooth d'un téléphone
//const char* BT_NAME = "SoundPort_30";

#define BT_LED 33
volatile uint32_t lastSoundMs = 0;     // dernier instant où du son a été reçu

#define I2S_BCK   26                           // <- GPIO 5 de l'ESP32-S3 (BCLK)
#define I2S_WS    25                           // <- GPIO 6 de l'ESP32-S3 (LRC)
#define I2S_DATA  27                           // <- GPIO 4 de l'ESP32-S3 (DOUT)

#define SWAP_LR   0                            // mets 1 si gauche et droite sont inversées
// =====================================================

const size_t RING_BYTES = 32768;               // tampon (~185 ms à 44,1 kHz)
const size_t RING_HIGH  = 20000;               // au-delà : on jette un bloc (limite le retard)
const size_t PREFILL    = 8192;                // reprise après un silence (~46 ms)

BluetoothA2DPSource a2dp_source;
StreamBufferHandle_t ring = nullptr;

volatile bool     buffering = true;
volatile uint32_t rxBytes = 0, dropBytes = 0, underruns = 0;

// ---------------- I2S en esclave ----------------
#if ESP_ARDUINO_VERSION_MAJOR >= 3

static i2s_chan_handle_t rx_handle = NULL;

void i2sInit() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
  chan_cfg.dma_desc_num  = 8;
  chan_cfg.dma_frame_num = 256;
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

  i2s_std_config_t std_cfg = {};
  std_cfg.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100);
  std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.bclk = (gpio_num_t)I2S_BCK;
  std_cfg.gpio_cfg.ws   = (gpio_num_t)I2S_WS;
  std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.din  = (gpio_num_t)I2S_DATA;
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
}

esp_err_t i2sRead(uint8_t* buf, size_t len, size_t* got) {
  return i2s_channel_read(rx_handle, buf, len, got, 200);        // timeout en ms
}

#else

void i2sInit() {
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_SLAVE | I2S_MODE_RX);
  cfg.sample_rate          = 44100;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 8;
  cfg.dma_buf_len          = 256;
  cfg.use_apll             = false;
  ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL));

  i2s_pin_config_t pins = {};
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = I2S_BCK;
  pins.ws_io_num    = I2S_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = I2S_DATA;
  ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pins));
}

esp_err_t i2sRead(uint8_t* buf, size_t len, size_t* got) {
  return i2s_read(I2S_NUM_0, buf, len, got, pdMS_TO_TICKS(200));  // timeout en ticks
}

#endif

// ---------------- Tâche : I2S -> tampon ----------------
void i2sReaderTask(void*) {
  static uint8_t buf[1024];
  for (;;) {
    size_t got = 0;
    esp_err_t r = i2sRead(buf, sizeof(buf), &got);
    if (r != ESP_OK) { vTaskDelay(1); continue; }
    got &= ~(size_t)3;                                   // trames complètes de 4 octets
    if (got == 0) continue;
    rxBytes += got;

    uint32_t* w = (uint32_t*)buf;                        // y a-t-il autre chose que des zéros ?
    for (size_t i = 0; i < got / 4; i++) {
      if (w[i]) { lastSoundMs = millis(); break; }
    }

#if SWAP_LR
    int16_t* s = (int16_t*)buf;
    for (size_t i = 0; i + 1 < got / 2; i += 2) { int16_t t = s[i]; s[i] = s[i + 1]; s[i + 1] = t; }
#endif

    if (xStreamBufferBytesAvailable(ring) > RING_HIGH) { dropBytes += got; continue; }
    xStreamBufferSend(ring, buf, got, 0);
  }
}

// ---------------- Rappel A2DP : tampon -> Bluetooth ----------------
int32_t get_data_frames(Frame* frame, int32_t frame_count) {
  size_t   need = (size_t)frame_count * 4;               // 2 voies x 16 bits
  uint8_t* out  = (uint8_t*)frame;

  if (buffering && xStreamBufferBytesAvailable(ring) >= PREFILL) buffering = false;

  if (!buffering) {
    size_t n = xStreamBufferReceive(ring, out, need, 0);
    if (n < need) {                                      // tampon vide : silence puis re-remplissage
      memset(out + n, 0, need - n);
      buffering = true;
      underruns++;
    }
  } else {
    memset(out, 0, need);
  }
  return frame_count;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("Passerelle I2S -> Bluetooth (A2DP)");

  pinMode(BT_LED, OUTPUT); digitalWrite(BT_LED, HIGH);

  ring = xStreamBufferCreate(RING_BYTES, 1);
  i2sInit();
  xTaskCreatePinnedToCore(i2sReaderTask, "i2sRx", 4096, nullptr, 5, nullptr, 1);

  a2dp_source.set_auto_reconnect(true);
  a2dp_source.start(BT_NAME, get_data_frames);
  Serial.printf("Recherche de l'enceinte : %s\n", BT_NAME);
}

void loop() {
  static uint32_t lastMs = 0, lastRx = 0;
  static bool wasConnected = false;

  bool connected = a2dp_source.is_connected();

  // ---- LED d'état ----
  static uint32_t ledT = 0;
  static bool ledOn = false;
  bool hasSound = (millis() - lastSoundMs) < 3000;
  uint32_t period;                                       // 0 = LED fixe
  if (!connected)     period = 500;                      // lent : recherche de l'enceinte
  else if (!hasSound) period = 100;                      // rapide : pas de son reçu
  else                period = 0;                        // fixe : tout va bien
  if (period == 0) ledOn = true;
  else if (millis() - ledT >= period) { ledT = millis(); ledOn = !ledOn; }
  digitalWrite(BT_LED, ledOn ? LOW : HIGH);  
  
  if (connected != wasConnected) {
    wasConnected = connected;
    Serial.println(connected ? "Enceinte connectee" : "Enceinte deconnectee");    
  }
//  if (wasConnected) { digitalWrite(BT_LED, LOW); } else { digitalWrite(BT_LED, HIGH); }

  uint32_t now = millis();
  if (now - lastMs >= 5000) {
    uint32_t rx   = rxBytes;
    uint32_t rate = (uint32_t)((uint64_t)(rx - lastRx) * 1000 / (now - lastMs));   // octets/s
    lastRx = rx; lastMs = now;
    Serial.printf("I2S: %u o/s (~%.1f kHz) | tampon %u | jetes %u | silences %u | BT %s\n",
                  (unsigned)rate, rate / 4000.0f,
                  (unsigned)xStreamBufferBytesAvailable(ring),
                  (unsigned)dropBytes, (unsigned)underruns,
                  connected ? "OK" : "non connecte");
  }
  delay(100);
}


//
