#include <driver/i2s.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include "FS.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// recording settings
#define SAMPLE_BUFFER_SIZE 512
#define SAMPLE_RATE 24000
#define RECORD_TIME_SECONDS 10
#define I2S_MIC_CHANNEL I2S_CHANNEL_FMT_ONLY_LEFT

// I2S pin config
#define I2S_MIC_SERIAL_CLOCK GPIO_NUM_32
#define I2S_MIC_LEFT_RIGHT_CLOCK GPIO_NUM_25
#define I2S_MIC_SERIAL_DATA GPIO_NUM_33

// total data size for 24-bit recording
#define FLASH_RECORD_SIZE (SAMPLE_RATE * 3 * RECORD_TIME_SECONDS) // 3 bytes per sample
#define BUFFER_SIZE 1024

// wifi
const char* ssid = "ssid_placeholder"; 
const char* password = "password_placeholder"; 
const char* serverURL = "http://192.168.1.100:8000/upload";  // local pc ip

// NTP settings for time sync
const char* ntpServer = "pool.ntp.org";
const char* ntpServer2 = "time.nist.gov";  // actually worked
const char* ntpServer3 = "time.google.com"; // google ntp server
const long gmtOffset_sec = 2 * 3600;        // Latvian timezone
const int daylightOffset_sec = 3600;    // daylight savings offset

struct Recording {
  String filename;
  time_t timestamp;
  String timeString;
};

String getTimeStr() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return "1970-01-01T00:00:00Z";
  }
  
  char timeString[64];
  strftime(timeString, sizeof(timeString), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
  return String(timeString);
}

time_t getTime() {
  time_t now;
  time(&now);
  return now;
}

void saveRecordingMetadata(const Recording& info) {
  String metadataFile = info.filename;
  metadataFile.replace(".wav", ".json"); // creating a json file with the same name as the recording
  
  File file = SD.open(metadataFile, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to create metadata file");
    return;
  }

  DynamicJsonDocument doc(256);
  doc["timestamp"] = info.timestamp;
  doc["time_string"] = info.timeString;
  
  serializeJson(doc, file);
  file.close();
  
  Serial.printf("Metadata saved: %s\n", metadataFile.c_str());
}

Recording loadRecordingMetadata(const String& wavFilename) {
  Recording info;
  info.filename = wavFilename;
  info.timestamp = 0;
  info.timeString = "unknown";
  
  String metadataFile = wavFilename;
  metadataFile.replace(".wav", ".json");
  
  File file = SD.open(metadataFile);
  if (!file) {
    Serial.printf("No metadata file found for %s\n", wavFilename.c_str());
    return info;
  }
  
  DynamicJsonDocument doc(256);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    Serial.printf("Failed to parse metadata: %s\n", error.c_str());
    return info;
  }
  
  info.timestamp = doc["timestamp"];
  info.timeString = doc["time_string"].as<String>();
  
  return info;
}

void setupI2S() {
  esp_err_t err;
  i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_24BIT,
      .channel_format = I2S_MIC_CHANNEL,
      .communication_format = I2S_COMM_FORMAT_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 4,
      .dma_buf_len = 1024,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0
  };

  i2s_pin_config_t i2s_mic_pins = {
      .bck_io_num = I2S_MIC_SERIAL_CLOCK,
      .ws_io_num = I2S_MIC_LEFT_RIGHT_CLOCK,
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num = I2S_MIC_SERIAL_DATA
  };

  err = i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("Failed to install I2S driver: %d\n", err);
  } else {
    Serial.println("I2S driver installed successfully");
  }

  err = i2s_set_pin(I2S_NUM_1, &i2s_mic_pins);
  if (err != ESP_OK) {
    Serial.printf("Failed to set I2S pins: %d\n", err);
  } else {
    Serial.println("I2S pins configured successfully");
  }
}
void writeWavHeader(File &file, uint32_t sampleRate, uint32_t dataSize) {
  uint32_t chunkSize = 36 + dataSize;
  uint16_t audioFormat = 1;        // PCM
  uint16_t numChannels = 1;        // Mono
  uint16_t bitsPerSample = 24;     // 24-bit
  uint16_t blockAlign = numChannels * (bitsPerSample / 8);
  uint32_t byteRate = sampleRate * blockAlign;
  uint32_t subchunk1Size = 16;     // PCM

  file.seek(0);  // go to start of file

  file.write((const uint8_t *)"RIFF", 4);
  file.write((uint8_t *)&chunkSize, 4);
  file.write((const uint8_t *)"WAVE", 4);

  file.write((const uint8_t *)"fmt ", 4);
  file.write((uint8_t *)&subchunk1Size, 4);
  file.write((uint8_t *)&audioFormat, 2);
  file.write((uint8_t *)&numChannels, 2);
  file.write((uint8_t *)&sampleRate, 4);
  file.write((uint8_t *)&byteRate, 4);
  file.write((uint8_t *)&blockAlign, 2);
  file.write((uint8_t *)&bitsPerSample, 2);

  file.write((const uint8_t *)"data", 4);
  file.write((uint8_t *)&dataSize, 4);
}

Recording recordWavFile(fs::FS &fs, const char* filename) {
  Recording recordingInfo;
  recordingInfo.filename = String(filename);
  recordingInfo.timestamp = getTime();
  recordingInfo.timeString = getTimeStr();

  Serial.printf("Recording WAV file: %s\n", filename);
  
  File file = fs.open(filename, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open file for writing");
    return recordingInfo;
  }

  // write initial WAV header
  writeWavHeader(file, SAMPLE_RATE, FLASH_RECORD_SIZE);

  uint32_t totalSamples = SAMPLE_RATE * RECORD_TIME_SECONDS;
  uint32_t samplesRecorded = 0;
  
  // buffer for I2S data (4 bytes/sample when reading 24-bit)
  uint8_t i2sBuffer[BUFFER_SIZE];
  uint8_t outputBuffer[BUFFER_SIZE * 3 / 4]; // 3 bytes per sample output
  
  Serial.println("Starting recording...");
  
  while (samplesRecorded < totalSamples) {
    size_t bytesRead = 0;
    esp_err_t result = i2s_read(I2S_NUM_1, i2sBuffer, BUFFER_SIZE, &bytesRead, portMAX_DELAY);
    
    if (result != ESP_OK) {
      Serial.printf("I2S read error: %d\n", result);
      break;
    }

    if (bytesRead == 0) {
      continue;
    }

    // 32bit -> 24bit
    uint32_t samplesInBuffer = bytesRead / 4;
    uint32_t outputIndex = 0;
    
    for (uint32_t i = 0; i < samplesInBuffer && samplesRecorded < totalSamples; i++) {
      int32_t sample32 = *((int32_t*)&i2sBuffer[i * 4]);
      int32_t sample24 = sample32 >> 8;
      sample24 = sample24 & 0xFFFFFF;  // Mask to 24 bits
      
      outputBuffer[outputIndex++] = (sample24 & 0xFF);
      outputBuffer[outputIndex++] = ((sample24 >> 8) & 0xFF);
      outputBuffer[outputIndex++] = ((sample24 >> 16) & 0xFF);
      
      samplesRecorded++;
    }
    
    // write the 24-bit data to file
    if (outputIndex > 0) {
      file.write(outputBuffer, outputIndex);
    }
  }

  Serial.printf("Recording complete. Samples recorded: %d\n", samplesRecorded);
  
  // update WAV header with actual data size
  uint32_t actualDataSize = samplesRecorded * 3; // 3 bytes per sample
  file.seek(0);
  writeWavHeader(file, SAMPLE_RATE, actualDataSize);
  
  file.close();

  saveRecordingMetadata(recordingInfo);
  return recordingInfo;
}

bool uploadFile(const Recording& info) {
  File file = SD.open(info.filename);
  if (!file) {
    Serial.println("Failed to open file for upload.");
    return false;
  }

  WiFiClient client;
  if (!client.connect("192.168.1.102", 8000)) {
    Serial.println("Connection failed");
    file.close();
    return false;
  }

  // streaming file to server
  String boundary = "----WebKitFormBoundary" + String(random(0xFFFFFFFF), HEX);
  String endBoundary = "\r\n--" + boundary + "--\r\n";

  // metadata json
  String metadataPart = "--" + boundary + "\r\n";
  metadataPart += "Content-Disposition: form-data; name=\"metadata\"\r\n";
  metadataPart += "Content-Type: application/json\r\n\r\n";
  metadataPart += "{\"timestamp\":" + String(info.timestamp) + ",\"time_string\":\"" + info.timeString + "\"}\r\n";

  String fileHeader = "--" + boundary + "\r\n";
  fileHeader += "Content-Disposition: form-data; name=\"audio\"; filename=\"" + info.filename + "\"\r\n";
  fileHeader += "Content-Type: audio/wav\r\n\r\n";

  // calculate total length
  size_t totalLength = metadataPart.length() + fileHeader.length() + file.size() + endBoundary.length();

  // start HTTP request
  client.printf("POST /upload HTTP/1.1\r\n");
  client.printf("Host: 192.168.1.102\r\n");
  client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary.c_str());
  client.printf("Content-Length: %u\r\n", totalLength);
  client.print("Connection: close\r\n\r\n");
  client.print(metadataPart);
  client.print(fileHeader);

  // stream wav file
  uint8_t buf[1024];
  while (file.available()) {
    size_t len = file.read(buf, sizeof(buf));
    client.write(buf, len);
  }
  file.close();

  client.print(endBoundary);

  // read server response
  bool success = false;
  unsigned long timeout = millis();
  while (client.connected() && millis() - timeout < 5000) {
    while (client.available()) {
      String line = client.readStringUntil('\n');
      Serial.println(line);
      if (line.startsWith("HTTP/1.1 200")) {
        success = true;
      }
    }
  }

  return success;
}

void deleteAllFiles(fs::FS &fs, const char * path) {
  File dir = fs.open(path);
  if (!dir || !dir.isDirectory()) {
    Serial.println("Failed to open directory for deletion");
    return;
  }

  File file = dir.openNextFile();
  while (file) {
    String fileName = file.name();
    if (file.isDirectory()) {
      file.close();
      deleteAllFiles(fs, fileName.c_str());
    } else {
      Serial.printf("Deleting file: %s\n", fileName.c_str());
      fs.remove(fileName.c_str());
      file.close();
    }
    file = dir.openNextFile();
  }
  dir.close();
}

void setup() {
  Serial.begin(115200);
  setupI2S();

#ifdef REASSIGN_PINS
  SPI.begin(sck, miso, mosi, cs);
  if (!SD.begin(cs)) {
#else
  if (!SD.begin()) {
#endif
    Serial.println("SD Card Mount Failed");
    return;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No SD card attached");
    return;
  }

  Serial.println("SD card initialized successfully");
  
  // delete all old files
  deleteAllFiles(SD, "/");

  // WiFi setup
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nWiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());

  // Initialize time
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer2);
  Serial.println("Waiting for NTP time sync...");
  
  // wait for time to be set
  int attempts = 0;
  while (time(nullptr) < 100000 && attempts < 60) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }
  Serial.println();
  
  if (time(nullptr) < 100000) {
    Serial.println("Failed to sync time, will use system time");
  } else {
    Serial.printf("Time synchronized: %s\n", getTimeStr().c_str());
  }
}

void loop() {
  static int fileIndex = 0;
  char filename[32];
  sprintf(filename, "/recording_%03d.wav", fileIndex++);

  Serial.printf("\n=== Recording #%d ===\n", fileIndex);
  Recording Recording = recordWavFile(SD, filename);

  // if upload fails, try sending all saved files later
  if (uploadFile(Recording)) {
    SD.remove(filename); // if upload succeeds, delete current file from SD

    String metadataFile = String(filename);
    metadataFile.replace(".wav", ".json");
    SD.remove(metadataFile.c_str()); // remove metadata file as well

    Serial.printf("Upload successful, deleted: %s\n", filename);
  } else {
    Serial.printf("Upload failed, keeping: %s\n", filename);
  }

  delay(1000);
}
