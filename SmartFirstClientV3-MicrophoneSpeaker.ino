#include <driver/i2s.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <HTTPClient.h>

// --- WIFI CONFIGURATION ---
#define WIFI_SSID     "Coop's iPhone"   // [cite: 1]
#define WIFI_PASSWORD "SAints80!"       // [cite: 1]
static const char* SERVER_BASE_URL = "http://172.20.10.2:1234"; // [cite: 2, 60]

// --- PINS & PORTS ---
#define PTT_PIN   27  // [cite: 6]
#define MIC_WS    25  // [cite: 6]
#define MIC_SD    33  // [cite: 6]
#define MIC_SCK   26  // [cite: 6]
#define SPK_DOUT  14  // [cite: 61]
#define SPK_BCLK  32  // [cite: 61]
#define SPK_LRC   22  // [cite: 61]

#define I2S_MIC_PORT I2S_NUM_0 // [cite: 6]
#define I2S_SPK_PORT I2S_NUM_1 // Using separate port for stability

// --- AUDIO SETTINGS ---
static const uint32_t MIC_RATE = 16000; // [cite: 3]
static const uint32_t SPK_RATE = 22050; // [cite: 62]
#define BUFFER_LEN 512

int32_t i2sSamples[BUFFER_LEN]; // [cite: 4]
int16_t pcmSamples[BUFFER_LEN]; 

// --- WAV HELPERS ---
static void write_le16(File &f, uint16_t v) {
  uint8_t b[2] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF) };
  f.write(b, 2); // [cite: 8]
}
static void write_le32(File &f, uint32_t v) {
  uint8_t b[4] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF), (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF) };
  f.write(b, 4); // [cite: 9]
}

void write_wav_header_placeholder(File &f) {
  f.seek(0);
  f.write((const uint8_t*)"RIFF", 4); // [cite: 11]
  write_le32(f, 36); 
  f.write((const uint8_t*)"WAVE", 4);
  f.write((const uint8_t*)"fmt ", 4);
  write_le32(f, 16);
  write_le16(f, 1); // PCM [cite: 12]
  write_le16(f, 1); // Mono [cite: 3]
  write_le32(f, MIC_RATE);
  write_le32(f, MIC_RATE * 2);
  write_le16(f, 2);
  write_le16(f, 16);
  f.write((const uint8_t*)"data", 4);
  write_le32(f, 0); // [cite: 13]
}

void finalize_wav_header(File &f, uint32_t dataSizeBytes) {
  f.seek(4);
  write_le32(f, 36 + dataSizeBytes); // [cite: 14]
  f.seek(40);
  write_le32(f, dataSizeBytes);
  f.flush();
}

// bool uploadFileMultipartRaw(const char* url, const char* pathOnFs) {
//   File f = LittleFS.open(pathOnFs, "r");
//   if (!f || f.size() == 0) {
//     Serial.println("Error: File is empty or doesn't exist!");
//     if (f) f.close();
//     return false;
//   }

//   WiFiClient client;
//   // Use the IP address directly to avoid DNS overhead
//   if (!client.connect("172.20.10.2", 1234)) { 
//     Serial.println("Connection failed");
//     f.close(); 
//     return false;
//   }

//   uint32_t fileSize = f.size();
//   Serial.printf("Uploading %d bytes from %s...\n", fileSize, pathOnFs);

//   // Send standard HTTP headers
//   client.print("POST /upload HTTP/1.1\r\n");
//   client.print("Host: 172.20.10.2\r\n");
//   client.print("Content-Type: audio/wav\r\n");
//   client.print("Content-Length: " + String(fileSize) + "\r\n");
//   client.print("Connection: close\r\n\r\n"); // Force connection closure after transfer

//   uint8_t buf[1024];
//   while (f.available()) {
//     size_t n = f.read(buf, sizeof(buf));
//     client.write(buf, n);
//     yield(); // Keep WiFi stack alive
//   }
//   f.close();

//   // --- CRITICAL: Wait for and read the server response ---
//   // Without this, the next connection attempt may fail because the socket is "dirty"
//   unsigned long timeout = millis();
//   while (client.connected() && millis() - timeout < 3000) {
//     if (client.available()) {
//       String line = client.readStringUntil('\n');
//       if (line.startsWith("HTTP/1.1 200")) {
//         Serial.println("Server confirmed receipt.");
//       }
//     }
//   }

//   client.stop(); // Properly release resources 
//   Serial.println("Upload complete and socket closed.");
//   return true;
// }

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
  }
  f.close();

  // READ RESPONSE
  String response = "";
  unsigned long uploadTimeout = millis(); // Renamed to avoid redeclaration error
  while (client.connected() && (millis() - uploadTimeout < 8000)) { // 8s for server to process
    if (client.available()) {
      response += client.readString();
    }
  }

  if (response.indexOf("TRIGGER_DOWNLOAD") >= 0) {
    Serial.println("Immediate Download Triggered!");
    downloadAndPlay(); 
  }

  client.stop();
  return true;
}

// --- SYSTEM SETUP ---
void setup() {
  Serial.begin(115200);
  pinMode(PTT_PIN, INPUT_PULLUP); // [cite: 39]
  if(!LittleFS.begin(true)) return;

  // Setup Mic (RX) [cite: 4]
  i2s_config_t mic_cfg = { .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX), .sample_rate = MIC_RATE, .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, .communication_format = I2S_COMM_FORMAT_I2S, .dma_buf_count = 4, .dma_buf_len = BUFFER_LEN };
  i2s_pin_config_t mic_pins = { .bck_io_num = MIC_SCK, .ws_io_num = MIC_WS, .data_out_num = I2S_PIN_NO_CHANGE, .data_in_num = MIC_SD };
  i2s_driver_install(I2S_MIC_PORT, &mic_cfg, 0, NULL);
  i2s_set_pin(I2S_MIC_PORT, &mic_pins);

  // Setup Speaker (TX) [cite: 62]
  i2s_config_t spk_cfg = { .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX), .sample_rate = SPK_RATE, .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, .communication_format = I2S_COMM_FORMAT_I2S_MSB, .dma_buf_count = 8, .dma_buf_len = 128 };
  i2s_pin_config_t spk_pins = { .bck_io_num = SPK_BCLK, .ws_io_num = SPK_LRC, .data_out_num = SPK_DOUT, .data_in_num = I2S_PIN_NO_CHANGE };
  i2s_driver_install(I2S_SPK_PORT, &spk_cfg, 0, NULL);
  i2s_set_pin(I2S_SPK_PORT, &spk_pins);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); } // [cite: 71]
  Serial.println("\nWiFi Connected.");
}

void loop() {
  static bool wasHeld = false;
  static bool recording = false;
  static File wavFile;
  static uint32_t dataBytes = 0;
  static unsigned long lastCheck = 0;

  bool held = (digitalRead(PTT_PIN) == LOW); // [cite: 7, 43]

  // Start Recording
  if (held && !wasHeld) {
    // 1. Force remove old file to free up blocks
    if (LittleFS.exists("/rec.wav")) {
        LittleFS.remove("/rec.wav");
    }
    
    wavFile = LittleFS.open("/rec.wav", "w");
    if (wavFile) {
        write_wav_header_placeholder(wavFile);
        dataBytes = 0;
        recording = true;
        Serial.println("Recording started...");
    } else {
        Serial.println("Critical Error: Could not open file for writing!");
        // If this happens, the FS is likely full or corrupt
        LittleFS.format(); 
    }
  }

  // Stop & Upload
  if (!held && wasHeld && recording) {
    finalize_wav_header(wavFile, dataBytes); // [cite: 48]
    wavFile.close();
    recording = false;
    Serial.println("Uploading...");
    uploadFileMultipartRaw((String(SERVER_BASE_URL) + "/upload").c_str(), "/rec.wav");
  }

  if (recording) {
    size_t bytes_read = 0;
    i2s_read(I2S_MIC_PORT, i2sSamples, sizeof(i2sSamples), &bytes_read, pdMS_TO_TICKS(10)); // [cite: 52]
    if (bytes_read > 0) {
      int samples = bytes_read / 4;
      for (int i = 0; i < samples; i++) {
        pcmSamples[i] = (int16_t)(i2sSamples[i] >> 16); // Convert 32-bit to 16-bit [cite: 55]
      }
      size_t written = wavFile.write((uint8_t*)pcmSamples, samples * 2);
      dataBytes += written; // [cite: 57]
    }
  } else if (millis() - lastCheck > 2000) { // Polling [cite: 72]
    lastCheck = millis();
    HTTPClient http;
    http.begin(String(SERVER_BASE_URL) + "/check_status"); // [cite: 73]
    if (http.GET() == 200 && http.getString() == "PLAY") {
      downloadAndPlay(); // [cite: 74]
    }
    http.end();
  }
  wasHeld = held;
}

void downloadAndPlay() {
  HTTPClient http;
  if (http.begin(String(SERVER_BASE_URL) + "/download")) { // [cite: 68]
    int code = http.GET();
    if (code == 200) {
      File f = LittleFS.open("/out.wav", "w");
      if (f) { 
        http.writeToStream(&f); // [cite: 70]
        f.close(); 
      }
      File play = LittleFS.open("/out.wav", "r");
      play.seek(44); // Skip WAV header [cite: 65]
      uint8_t data[512];
      size_t written;
      while (play.available()) {
        int r = play.read(data, 512); // [cite: 66]
        i2s_write(I2S_SPK_PORT, data, r, &written, portMAX_DELAY);
      }
      play.close();
      Serial.println("Done.");
    }
  }
}