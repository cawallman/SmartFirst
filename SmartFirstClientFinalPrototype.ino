#include <driver/i2s.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <HTTPClient.h>

// --- WIFI CONFIGURATION ---
#define WIFI_SSID     "Coop's iPhone"
#define WIFI_PASSWORD "SAints80!"
static const char* SERVER_BASE_URL = "http://172.20.10.2:1234";

// --- PINS & PORTS ---
#define PTT_PIN   27  
#define MIC_WS    25  
#define MIC_SD    33  
#define MIC_SCK   26  
#define SPK_DOUT  14  
#define SPK_BCLK  32  
#define SPK_LRC   22  

#define I2S_MIC_PORT I2S_NUM_0 
#define I2S_SPK_PORT I2S_NUM_1 

// --- AUDIO SETTINGS ---
static const uint32_t MIC_RATE = 16000;
static const uint32_t SPK_RATE = 22050; 
#define BUFFER_LEN 512

int32_t i2sSamples[BUFFER_LEN]; 
int16_t pcmSamples[BUFFER_LEN]; 

// --- WAV HELPERS (For Recording Only) ---
static void write_le16(File &f, uint16_t v) {
  uint8_t b[2] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF) };
  f.write(b, 2);
}
static void write_le32(File &f, uint32_t v) {
  uint8_t b[4] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF), (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF) };
  f.write(b, 4);
}

void write_wav_header_placeholder(File &f) {
  f.seek(0);
  f.write((const uint8_t*)"RIFF", 4);
  write_le32(f, 36); 
  f.write((const uint8_t*)"WAVE", 4);
  f.write((const uint8_t*)"fmt ", 4);
  write_le32(f, 16);
  write_le16(f, 1); // PCM
  write_le16(f, 1); // Mono
  write_le32(f, MIC_RATE);
  write_le32(f, MIC_RATE * 2);
  write_le16(f, 2);
  write_le16(f, 16);
  f.write((const uint8_t*)"data", 4);
  write_le32(f, 0);
}

void finalize_wav_header(File &f, uint32_t dataSizeBytes) {
  f.seek(4);
  write_le32(f, 36 + dataSizeBytes);
  f.seek(40);
  write_le32(f, dataSizeBytes);
  f.flush();
}

// --- STREAMING LOGIC (The Space Fix) ---
void streamAndPlay() {
  HTTPClient http;
  String url = String(SERVER_BASE_URL) + "/download";
  
  if (http.begin(url)) {
    int httpCode = http.GET();
    if (httpCode == 200) {
      Serial.println("Streaming confirmed. Hard-resetting I2S...");

      // 1. COMPLETELY UNINSTALL EVERYTHING TO RELEASE PINS
      i2s_driver_uninstall(I2S_MIC_PORT);
      i2s_driver_uninstall(I2S_SPK_PORT);
      delay(20); // Small breath for the hardware

      // 2. RE-INSTALL SPEAKER DRIVER
      i2s_config_t spk_cfg = {
          .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
          .sample_rate = 22050,
          .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
          .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
          .communication_format = I2S_COMM_FORMAT_I2S,
          .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
          .dma_buf_count = 8,
          .dma_buf_len = 128,
          .tx_desc_auto_clear = true
      };
      
      i2s_pin_config_t spk_pins = {
          .bck_io_num = 32,
          .ws_io_num = 22,
          .data_out_num = 14,
          .data_in_num = -1 // Explicitly disable input
      };

      i2s_driver_install(I2S_SPK_PORT, &spk_cfg, 0, NULL);
      i2s_set_pin(I2S_SPK_PORT, &spk_pins);
      i2s_zero_dma_buffer(I2S_SPK_PORT);

      WiFiClient* stream = http.getStreamPtr();
      
      // 3. Skip header and stream
      uint8_t headerTrash[44];
      stream->readBytes(headerTrash, 44);

      uint8_t buffer[512];
      size_t totalBytes = 0;
      while (http.connected() && (stream->available() || stream->connected())) {
        if (stream->available()) {
          size_t size = stream->readBytes(buffer, sizeof(buffer));
          if (size > 0) {
            size_t bytesWritten;
            i2s_write(I2S_SPK_PORT, buffer, size, &bytesWritten, portMAX_DELAY);
            totalBytes += size;
          }
        }
        yield();
      }
      
      Serial.printf("Stream finished. Total bytes: %d\n", totalBytes);

      // 4. CLEAN UP AND RESTORE MIC
      i2s_driver_uninstall(I2S_SPK_PORT);
      setupMic(); // We will move mic setup to a helper function
    }
    http.end();
  }
}

bool uploadFileMultipartRaw(const char* url, const char* pathOnFs) {
  File f = LittleFS.open(pathOnFs, "r");
  if (!f || f.size() == 0) {
    Serial.println("Error: File is empty!");
    if (f) f.close();
    return false;
  }

  WiFiClient client;
  if (!client.connect("172.20.10.2", 1234)) { 
    f.close(); 
    return false;
  }

  uint32_t fileSize = f.size();
  client.print("POST /upload HTTP/1.1\r\n");
  client.print("Host: 172.20.10.2\r\n");
  client.print("Content-Type: audio/wav\r\n");
  client.print("Content-Length: " + String(fileSize) + "\r\n");
  client.print("Connection: close\r\n\r\n");

  uint8_t buf[1024];
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    client.write(buf, n);
    yield();
  }
  f.close();

  // READ RESPONSE (Increased timeout for AWS)
  String response = "";
  unsigned long uploadTimeout = millis();
  while (client.connected() && (millis() - uploadTimeout < 40000)) { 
    if (client.available()) {
      response += client.readString();
    }
  }

  if (response.indexOf("TRIGGER_DOWNLOAD") >= 0) {
    Serial.println("Server ready. Starting stream...");
    streamAndPlay(); 
  }

  client.stop();
  return true;
}

void setupMic() {
  i2s_config_t mic_cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = MIC_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .dma_buf_count = 4,
    .dma_buf_len = BUFFER_LEN
  };
  i2s_pin_config_t mic_pins = {
    .bck_io_num = MIC_SCK,
    .ws_io_num = MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_SD
  };
  i2s_driver_install(I2S_MIC_PORT, &mic_cfg, 0, NULL);
  i2s_set_pin(I2S_MIC_PORT, &mic_pins);
}

void setup() {
  Serial.begin(115200);
  pinMode(PTT_PIN, INPUT_PULLUP);
  LittleFS.begin(true);
  
  setupMic(); // Use the helper
  
  // Note: We don't need to install the Speaker here anymore, 
  // because streamAndPlay will do it when needed.

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nReady.");
}

void loop() {
  static bool wasHeld = false;
  static bool recording = false;
  static File wavFile;
  static uint32_t dataBytes = 0;
  static unsigned long lastCheck = 0;

  bool held = (digitalRead(PTT_PIN) == LOW);

  // START RECORDING
  if (held && !wasHeld) {
    if (wavFile) wavFile.close();
    LittleFS.remove("/rec.wav");
    
    wavFile = LittleFS.open("/rec.wav", "w");
    if (wavFile) {
        write_wav_header_placeholder(wavFile);
        dataBytes = 0;
        recording = true;
        Serial.println("Recording...");
    }
  }

  // STOP & UPLOAD
  if (!held && wasHeld && recording) {
    finalize_wav_header(wavFile, dataBytes);
    wavFile.close();
    recording = false;
    Serial.println("Sending to AWS...");
    uploadFileMultipartRaw((String(SERVER_BASE_URL) + "/upload").c_str(), "/rec.wav");
  }

  if (recording) {
    size_t bytes_read = 0;
    i2s_read(I2S_MIC_PORT, i2sSamples, sizeof(i2sSamples), &bytes_read, pdMS_TO_TICKS(10));
    if (bytes_read > 0) {
      int samples = bytes_read / 4;
      for (int i = 0; i < samples; i++) {
        pcmSamples[i] = (int16_t)(i2sSamples[i] >> 16);
      }
      dataBytes += wavFile.write((uint8_t*)pcmSamples, samples * 2);
    }
  } else if (millis() - lastCheck > 2000) {
    lastCheck = millis();
    HTTPClient http;
    http.begin(String(SERVER_BASE_URL) + "/check_status");
    if (http.GET() == 200 && http.getString() == "PLAY") {
      streamAndPlay();
    }
    http.end();
  }
  wasHeld = held;
}