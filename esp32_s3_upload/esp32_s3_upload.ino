/*
  INMP441 -> WiFi -> S3 (URL prefirmada)
  --------------------------------------
  Cableado:
    VDD -> 3.3V | GND -> GND | SCK -> GPIO32 | WS -> GPIO25 | SD -> GPIO33 | L/R -> GND

  Funcionamiento:
    - Pulsa el boton BOOT (GPIO0) y el ESP32 graba RECORD_SECONDS segundos.
    - El audio se envia en vivo a S3 como WAV (sin guardarlo en RAM).
    - Un Lambda en AWS detecta el archivo nuevo y lo transcribe.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <driver/i2s.h>

// ---------- CONFIGURA ESTO ----------
const char* WIFI_SSID = "UAM";
const char* WIFI_PASS = "";

// De la URL prefirmada:  https://<S3_HOST><S3_PATH>
const char* S3_HOST = "mi-bucket.s3.us-east-1.amazonaws.com";
const char* S3_PATH = "/audio/esp32.wav?X-Amz-Algorithm=...&X-Amz-Signature=...";

#define RECORD_SECONDS 10
// ------------------------------------

#define I2S_PORT      I2S_NUM_0
#define I2S_SCK       32
#define I2S_WS        25
#define I2S_SD        33
#define SAMPLE_RATE   16000
#define BUF_SAMPLES   256
#define SHIFT         14      // 16 = sin ganancia, 14 = x4, 12 = x16
#define BTN_PIN       0       // boton BOOT

int32_t raw[BUF_SAMPLES];
int16_t out[BUF_SAMPLES];

void setupI2S() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 16,     // buffer grande para aguantar pausas de WiFi/TLS
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  i2s_pin_config_t pins = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };
  i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
  i2s_set_pin(I2S_PORT, &pins);
  i2s_zero_dma_buffer(I2S_PORT);
}

void writeLE32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
void writeLE16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

void makeWavHeader(uint8_t* h, uint32_t dataBytes) {
  memcpy(h, "RIFF", 4);          writeLE32(h + 4, 36 + dataBytes);
  memcpy(h + 8, "WAVEfmt ", 8);  writeLE32(h + 16, 16);
  writeLE16(h + 20, 1);          // PCM
  writeLE16(h + 22, 1);          // mono
  writeLE32(h + 24, SAMPLE_RATE);
  writeLE32(h + 28, SAMPLE_RATE * 2);
  writeLE16(h + 32, 2);          // block align
  writeLE16(h + 34, 16);         // bits
  memcpy(h + 36, "data", 4);     writeLE32(h + 40, dataBytes);
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Conectando WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.print(" OK, IP: ");
  Serial.println(WiFi.localIP());
}

bool recordAndUpload() {
  const uint32_t dataBytes = (uint32_t)SAMPLE_RATE * RECORD_SECONDS * 2;
  const uint32_t totalSamples = (uint32_t)SAMPLE_RATE * RECORD_SECONDS;

  WiFiClientSecure client;
  client.setInsecure();   // para produccion, usa client.setCACert(AmazonRootCA1)
  client.setTimeout(15);

  Serial.println("Conectando a S3...");
  if (!client.connect(S3_HOST, 443)) {
    Serial.println("No se pudo conectar a S3");
    return false;
  }

  // Peticion PUT con Content-Length conocido (S3 no acepta chunked)
  client.print("PUT ");  client.print(S3_PATH);  client.print(" HTTP/1.1\r\n");
  client.print("Host: "); client.print(S3_HOST); client.print("\r\n");
  client.print("Content-Length: "); client.print(44 + dataBytes); client.print("\r\n");
  client.print("Connection: close\r\n\r\n");

  uint8_t header[44];
  makeWavHeader(header, dataBytes);
  client.write(header, 44);

  i2s_zero_dma_buffer(I2S_PORT);
  i2s_start(I2S_PORT);
  Serial.println("Grabando y enviando...");

  uint32_t sent = 0;
  while (sent < totalSamples) {
    size_t bytesRead = 0;
    i2s_read(I2S_PORT, raw, sizeof(raw), &bytesRead, portMAX_DELAY);
    uint32_t n = bytesRead / sizeof(int32_t);
    if (n > totalSamples - sent) n = totalSamples - sent;

    for (uint32_t i = 0; i < n; i++) {
      int32_t s = raw[i] >> SHIFT;
      if (s > 32767) s = 32767;
      if (s < -32768) s = -32768;
      out[i] = (int16_t)s;
    }
    if (client.write((uint8_t*)out, n * 2) != n * 2) {
      Serial.println("Error enviando datos");
      i2s_stop(I2S_PORT);
      client.stop();
      return false;
    }
    sent += n;
  }
  i2s_stop(I2S_PORT);

  // Leer respuesta de S3
  String status = client.readStringUntil('\n');
  client.stop();
  Serial.print("Respuesta: ");
  Serial.println(status);
  return status.indexOf(" 200") > 0;
}

void setup() {
  Serial.begin(115200);
  pinMode(BTN_PIN, INPUT_PULLUP);
  setupI2S();
  i2s_stop(I2S_PORT);
  connectWiFi();
  Serial.println("Listo. Pulsa BOOT para grabar.");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  if (digitalRead(BTN_PIN) == LOW) {
    delay(30);
    if (digitalRead(BTN_PIN) == LOW) {
      Serial.println(recordAndUpload() ? "Subido a S3 :)" : "Fallo la subida :(");
      while (digitalRead(BTN_PIN) == LOW) delay(10);   // espera a soltar
      Serial.println("Listo. Pulsa BOOT para grabar.");
    }
  }
  delay(10);
}
