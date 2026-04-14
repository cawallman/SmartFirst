#include <driver/i2s.h>
#include <FS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <HTTPClient.h>

// --- WIFI CONFIGURATION ---
#define WIFI_SSID     "Coop's iPhone"
#define WIFI_PASSWORD "SAints80!"
static const char* SERVER_BASE_URL = "http://172.20.10.2:1234";

bool isFirstUpload = true;

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
      // i2s_driver_uninstall(I2S_SPK_PORT);
      delay(20); // Small breath for the hardware

      // 2. RE-INSTALL SPEAKER DRIVER
      i2s_config_t spk_cfg = {
          .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
          .sample_rate = 22050,
          .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
          .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
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
  if (!f) {
    Serial.println("Failed to open file for reading");
    return false;
  }
  
  uint32_t fileSize = f.size();
  WiFiClient client;

  // Connecting to the server
  if (!client.connect("172.20.10.2", 1234)) {
    Serial.println("Connection to server failed");
    f.close();
    return false;
  }

  // 1. Send HTTP POST Headers
  client.print("POST /upload HTTP/1.1\r\n");
  client.print("Host: 172.20.10.2\r\n");
  client.print("Content-Type: audio/wav\r\n");
  client.printf("Content-Length: %u\r\n", fileSize);

  if (isFirstUpload) {
    client.print("X-Reset-Session: true\r\n");
    isFirstUpload = false; // Set to false so subsequent uploads stay in the same session
  }

  client.print("Connection: close\r\n\r\n");

  // 2. Stream the WAV file data directly to the socket [cite: 29]
  size_t sent = client.write(f);
  Serial.printf("Sent %d bytes to server. Processing...\n", sent);
  f.close();

  // 3. Robust Wait Logic for "TRIGGER_DOWNLOAD" 
  // We use a longer timeout (20s) because Speech-to-Text and APIs take time
  String response = "";
  unsigned long startWait = millis();
  bool triggerFound = false;

  while (client.connected() && (millis() - startWait < 20000)) {
    while (client.available()) {
      char c = client.read();
      response += c;
      
      // If we see the trigger, we can stop waiting and start playback
      if (response.indexOf("TRIGGER_DOWNLOAD") >= 0) {
        Serial.println("Trigger received! Starting playback...");
        triggerFound = true;
        break;
      }
    }
    if (triggerFound) break;
    yield(); // Keep the ESP32 watchdog happy
  }

  // 4. Handle Playback or Errors
  if (triggerFound) {
    streamAndPlay();
  } else {
    if (millis() - startWait >= 20000) {
      Serial.println("Error: Server timed out during processing.");
    } else {
      Serial.println("Error: Connection closed without trigger.");
    }
    // Debug: Print what the server actually sent if it wasn't the trigger
    Serial.println("Server Response: " + response);
  }

  client.stop();
  return triggerFound;
}

void setupMic() {
  // Ensure the port is clean before starting
  i2s_driver_uninstall(I2S_MIC_PORT); 
  delay(10); 

  i2s_config_t mic_cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = MIC_RATE, // 16000 
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, // 
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // 
    // Crucial: Use I2S_MSB_FORMAT for most MEMS mics on newer ESP32 cores
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB), 
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = BUFFER_LEN // 512 [cite: 11]
  };

  i2s_pin_config_t mic_pins = {
    .bck_io_num = MIC_SCK, // 26 
    .ws_io_num = MIC_WS,   // 25 
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_SD  // 33 
  };

  i2s_driver_install(I2S_MIC_PORT, &mic_cfg, 0, NULL);
  i2s_set_pin(I2S_MIC_PORT, &mic_pins);
}

void setup() {
  Serial.begin(115200);
  pinMode(PTT_PIN, INPUT_PULLUP);
  LittleFS.begin(true);
  LittleFS.format();
  
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
    
    if (LittleFS.exists("/rec.wav")) {
      LittleFS.remove("/rec.wav");
    }
    
    wavFile = LittleFS.open("/rec.wav", "w");
    if (wavFile) {
        write_wav_header_placeholder(wavFile);
        dataBytes = 0; // Reset this globally! 
        recording = true;
        Serial.println("Recording started...");
    }
  }

  // STOP & UPLOAD
  if (!held && wasHeld && recording) {
    recording = false; // Stop recording immediately
    
    finalize_wav_header(wavFile, dataBytes);
    wavFile.close(); 
    
    // RE-OPEN to verify actual size
    File verify = LittleFS.open("/rec.wav", "r");
    uint32_t actualSize = verify.size();
    verify.close();

    Serial.printf("Final File Size on Disk: %u bytes\n", actualSize);
    Serial.println("Sending to Server...");
    
    // Pass the ACTUAL size to your upload function
    uploadFileMultipartRaw((String(SERVER_BASE_URL) + "/upload").c_str(), "/rec.wav");
  }

  if (recording) {
    size_t bytes_read = 0;
    i2s_read(I2S_MIC_PORT, i2sSamples, sizeof(i2sSamples), &bytes_read, pdMS_TO_TICKS(10));
    if (bytes_read > 0) {
      int samples = bytes_read / 4;
      for (int i = 0; i < samples; i++) {
        pcmSamples[i] = (int16_t)(i2sSamples[i] >> 14);
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