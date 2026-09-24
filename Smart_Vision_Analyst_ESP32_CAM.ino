#define BOARD_HAS_PSRAM
#include "esp_camera.h"
#include "img_converters.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "driver/i2s.h"
#include "mbedtls/base64.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#ifndef RTC_CNTL_BROWNOUT_REG
 #define RTC_CNTL_BROWNOUT_REG RTC_CNTL_BROWN_OUT_REG
#endif
// =========================================================================
//  CONFIGURATIONS & API KEYS
// =========================================================================
const char* ssid        = "WIFI_SSID";
const char* password    = "WIFI_PASSWORD";
const char* visionApiKey = "api_key_from_circuitdigest";             
const char* sarvamKey    = "tts_api_from_sarvam";
// Audio gain multiplier for digital volume boost (Range: 0.0f = Mute, 1.0f = Original, 2.5f = Loud/Clear, >3.5f = Distortion)
const float AUDIO_GAIN_FACTOR = 2.5f;
// --- PINS ---
#define CAPTURE_BTN_PIN 13    
#define LANG_BTN_PIN    2     
#define FLASH_LED_PIN   4    
// --- MAX98357A I2S PINS (Shared with Camera Data Lines) ---
#define I2S_BCLK       14
#define I2S_LRC        12
#define I2S_DOUT       15
#define I2S_NUM        I2S_NUM_0
// --- LANGUAGE CONFIGURATIONS ---
struct LanguageConfig {
 const char* code;         
 const char* voice;        
 const char* name;         
 const char* prompt;       
 const char* notifyText;   
 const char* wifiConnectedText;
};
// Optimized prompts under 12 words
LanguageConfig languages[] = {
 { "hi-IN", "shubh",   "Hindi",     "Describe obstacles directly ahead for a blind user in Hindi in under 15 words. State if path is clear or blocked.", "हिंदी भाषा चुनी गई है",                 "वाई-फाई कनेक्ट हो गया है" },
 { "en-IN", "shubh",   "English",   "Describe obstacles directly ahead for a blind user in English in under 15 words. State if path is clear or blocked.", "English language selected",            "Wi-Fi connected successfully" },
 { "ta-IN", "kavitha", "Tamil",     "Describe obstacles directly ahead for a blind user in Tamil in under 15 words. State if path is clear or blocked.", "தமிழ் மொழி தேர்ந்தெடுக்கப்பட்டது",     "வைஃபை இணைக்கப்பட்டது" },
 { "ml-IN", "gokul",   "Malayalam", "Describe obstacles directly ahead for a blind user in Malayalam in under 15 words. State if path is clear or blocked.", "മലയാളം ഭാഷ തിരഞ്ഞെടുത്തു",         "വൈഫൈ കണക്റ്റായി" }
};
const int TOTAL_LANGUAGES = 4;
int currentLangIndex = 0; 
const float speechPace = 0.90;
unsigned long lastBtnPressTime = 0;
bool i2sInitialized = false;
// --- CAMERA PINOUT ---
#define PWDN_GPIO_NUM  32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM   0
#define SIOD_GPIO_NUM  26
#define SIOC_GPIO_NUM  27
#define Y9_GPIO_NUM    35
#define Y8_GPIO_NUM    34
#define Y7_GPIO_NUM    39
#define Y6_GPIO_NUM    36
#define Y5_GPIO_NUM    21
#define Y4_GPIO_NUM    19
#define Y3_GPIO_NUM    18
#define Y2_GPIO_NUM     5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM  23
#define PCLK_GPIO_NUM  22
// Struct for benchmark timing calculations
struct TTSTiming {
 unsigned long tts_connect;
 unsigned long tts_post;
 unsigned long tts_download;
 unsigned long tts_extract;
 unsigned long tts_decode;
 unsigned long tts_playback;
};
void captureAndAnalyze();
bool speakWithSarvam(String text, const char* langCode, const char* voiceModel, TTSTiming &timing);
// --- DYNAMIC BUS SWITCHING (I2S ROUTINES) ---
void initI2S(uint32_t sampleRate) {
 if (i2sInitialized) {
   i2s_driver_uninstall(I2S_NUM);
   i2sInitialized = false;
 }
 i2s_config_t i2s_config = {
   .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
   .sample_rate = sampleRate,
   .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
   .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
   .communication_format = I2S_COMM_FORMAT_STAND_I2S,
   .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
   .dma_buf_count = 8,
   .dma_buf_len = 512,
   .use_apll = false,
   .tx_desc_auto_clear = true,
   .fixed_mclk = 0
 };
 i2s_pin_config_t pin_config = {
   .bck_io_num = I2S_BCLK,
   .ws_io_num = I2S_LRC,
   .data_out_num = I2S_DOUT,
   .data_in_num = I2S_PIN_NO_CHANGE
 };
 if (i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL) == ESP_OK) {
   i2s_set_pin(I2S_NUM, &pin_config);
   i2s_zero_dma_buffer(I2S_NUM);
   i2sInitialized = true;
 }
}
void stopI2S() {
 if (i2sInitialized) {
   i2s_zero_dma_buffer(I2S_NUM);
   delay(50);
   i2s_driver_uninstall(I2S_NUM);
   i2sInitialized = false;
 }
}
bool initCameraHardware() {
 stopI2S(); // Always stop I2S before grabbing GPIO 12, 14, 15 for camera
 camera_config_t config;
 config.ledc_channel = LEDC_CHANNEL_0;
 config.ledc_timer   = LEDC_TIMER_0;
 config.pin_d0       = Y2_GPIO_NUM;
 config.pin_d1       = Y3_GPIO_NUM;
 config.pin_d2       = Y4_GPIO_NUM;
 config.pin_d3       = Y5_GPIO_NUM;
 config.pin_d4       = Y6_GPIO_NUM;
 config.pin_d5       = Y7_GPIO_NUM;
 config.pin_d6       = Y8_GPIO_NUM;
 config.pin_d7       = Y9_GPIO_NUM;
 config.pin_xclk     = XCLK_GPIO_NUM;
 config.pin_pclk     = PCLK_GPIO_NUM;
 config.pin_vsync    = VSYNC_GPIO_NUM;
 config.pin_href     = HREF_GPIO_NUM;
 config.pin_sccb_sda = SIOD_GPIO_NUM;
 config.pin_sccb_scl = SIOC_GPIO_NUM;
 config.pin_pwdn     = PWDN_GPIO_NUM;
 config.pin_reset    = RESET_GPIO_NUM;
 config.xclk_freq_hz = 20000000;
 // NOTE: Changed from PIXFORMAT_JPEG. This sensor has no hardware JPEG
 // encoder, so we capture raw RGB565 and convert to JPEG in software
 // (see frame2jpg() call in captureAndAnalyze()).
 config.pixel_format = PIXFORMAT_RGB565;
 if (psramFound()) {
   config.frame_size   = FRAMESIZE_QVGA; 
   config.jpeg_quality = 12;
   config.fb_count     = 1;
   config.fb_location  = CAMERA_FB_IN_PSRAM;
 } else {
   config.frame_size   = FRAMESIZE_QVGA;
   config.jpeg_quality = 15;
   config.fb_count     = 1;
   config.fb_location  = CAMERA_FB_IN_DRAM;
 }
 config.grab_mode = CAMERA_GRAB_LATEST;
 esp_err_t err = esp_camera_init(&config);
 return (err == ESP_OK);
}
String extractBase64Audio(const String& jsonResponse) {
 int keyIndex = jsonResponse.indexOf("\"audios\":[\"");
 if (keyIndex == -1) keyIndex = jsonResponse.indexOf("\"audios\": [\"");
 if (keyIndex != -1) {
   int start = jsonResponse.indexOf("\"", keyIndex + 10) + 1;
   int end   = jsonResponse.indexOf("\"", start);
   if (start > 0 && end > start) {
     return jsonResponse.substring(start, end);
   }
 }
 return "";
}
unsigned long playWAVBuffer(uint8_t* wavData, size_t wavLen) {
 unsigned long startAudioPlay = millis();
 if (!wavData || wavLen <= 44) return 0;
 uint32_t sampleRate = 16000; 
 uint16_t numChannels = 1;     
 size_t headerOffset = 0;
 if (wavData[0] == 'R' && wavData[1] == 'I' && wavData[2] == 'F' && wavData[3] == 'F') {
   headerOffset = 44; 
   numChannels = wavData[22] | (wavData[23] << 8);
   sampleRate = wavData[24] | (wavData[25] << 8) | ((uint32_t)wavData[26] << 16) | ((uint32_t)wavData[27] << 24);
 }
 initI2S(sampleRate);
 size_t bytesWritten = 0;
 uint8_t* payload = wavData + headerOffset;
 size_t payloadLen = wavLen - headerOffset;
 if (numChannels == 1) {
   int16_t* monoSamples = (int16_t*)payload;
   size_t monoCount = payloadLen / 2;
   int16_t stereoChunk[512 * 2];
   for (size_t i = 0; i < monoCount; i += 512) {
     size_t framesToProcess = (monoCount - i < 512) ? (monoCount - i) : 512;
     for (size_t j = 0; j < framesToProcess; j++) {
       int32_t boostedSample = (int32_t)(monoSamples[i + j] * AUDIO_GAIN_FACTOR);
       if (boostedSample > 32767)  boostedSample = 32767;
       if (boostedSample < -32768) boostedSample = -32768;
       int16_t cleanSample = (int16_t)boostedSample;
       stereoChunk[j * 2]     = cleanSample;
       stereoChunk[j * 2 + 1] = cleanSample;
     }
     i2s_write(I2S_NUM, stereoChunk, framesToProcess * 4, &bytesWritten, portMAX_DELAY);
   }
 } else {
   int16_t* stereoSamples = (int16_t*)payload;
   size_t stereoCount = payloadLen / 2;
   int16_t chunk[512 * 2];
   for (size_t i = 0; i < stereoCount; i += 1024) {
     size_t samplesToProcess = (stereoCount - i < 1024) ? (stereoCount - i) : 1024;
     for (size_t j = 0; j < samplesToProcess; j++) {
       int32_t boosted = (int32_t)(stereoSamples[i + j] * AUDIO_GAIN_FACTOR);
       if (boosted > 32767)  boosted = 32767;
       if (boosted < -32768) boosted = -32768;
       chunk[j] = (int16_t)boosted;
     }
     i2s_write(I2S_NUM, chunk, samplesToProcess * 2, &bytesWritten, portMAX_DELAY);
   }
 }
 int16_t silence[256] = {0};
 i2s_write(I2S_NUM, silence, sizeof(silence), &bytesWritten, portMAX_DELAY);
 delay(50);
 stopI2S();
 return millis() - startAudioPlay;
}
bool speakWithSarvam(String text, const char* langCode, const char* voiceModel, TTSTiming &timing) {
 if (WiFi.status() != WL_CONNECTED) return false;
 WiFiClientSecure client;
 client.setInsecure();
 HTTPClient http;
 String url = "https://api.sarvam.ai/text-to-speech";
 unsigned long t0 = millis();
 if (!http.begin(client, url)) return false;
 timing.tts_connect = millis() - t0;
 http.setTimeout(12000);
 http.addHeader("api-subscription-key", sarvamKey);
 http.addHeader("Content-Type", "application/json");
 String jsonBody = "{";
 jsonBody += "\"inputs\":[\"" + text + "\"],";
 jsonBody += "\"target_language_code\":\"" + String(langCode) + "\",";
 jsonBody += "\"speaker\":\"" + String(voiceModel) + "\",";
 jsonBody += "\"pace\":" + String(speechPace, 2) + ",";
 jsonBody += "\"speech_sample_rate\":16000,";
 jsonBody += "\"enable_preprocessing\":true,";
 jsonBody += "\"model\":\"bulbul:v3\"";
 jsonBody += "}";
 t0 = millis();
 int httpCode = http.POST(jsonBody);
 timing.tts_post = millis() - t0;
 if (httpCode == HTTP_CODE_OK) {
   t0 = millis();
   String payload = http.getString();
   http.end();
   timing.tts_download = millis() - t0;
   t0 = millis();
   String b64Audio = extractBase64Audio(payload);
   timing.tts_extract = millis() - t0;
   if (b64Audio.length() == 0) return false;
   size_t b64Len = b64Audio.length();
   size_t maxOutLen = (b64Len * 3 / 4) + 256;
   uint8_t* rawBuffer = (uint8_t*)heap_caps_malloc(maxOutLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!rawBuffer) rawBuffer = (uint8_t*)malloc(maxOutLen);
   if (!rawBuffer) {
     Serial.println("[ERROR] Memory Allocation Failed!");
     return false;
   }
   t0 = millis();
   size_t outLen = 0;
   int ret = mbedtls_base64_decode(rawBuffer, maxOutLen, &outLen, 
                                  (const unsigned char*)b64Audio.c_str(), b64Len);
   timing.tts_decode = millis() - t0;
   if (ret == 0 && outLen > 0) {
     timing.tts_playback = playWAVBuffer(rawBuffer, outLen);
   }
   free(rawBuffer);
   return true;
 }
 http.end();
 return false;
}
void setup() {
 WRITE_PERI_REG(RTC_CNTL_BROWNOUT_REG, 0);
 Serial.begin(115200);
 pinMode(CAPTURE_BTN_PIN, INPUT_PULLUP);
 pinMode(LANG_BTN_PIN, INPUT_PULLUP); 
 pinMode(FLASH_LED_PIN, OUTPUT);
 digitalWrite(FLASH_LED_PIN, LOW);
 Serial.println("\n========================================");
 Serial.println("  ESP32-CAM Multi-Language Vision TTS  ");
 Serial.println("========================================\n");
 WiFi.begin(ssid, password);
 WiFi.setSleep(false);
 Serial.print("[WIFI] Connecting");
 while (WiFi.status() != WL_CONNECTED) {
   delay(500);
   Serial.print(".");
 }
 Serial.println("\n[WIFI OK] Connected!");
 TTSTiming dummyTiming = {0};
 LanguageConfig current = languages[currentLangIndex];
 speakWithSarvam(current.wifiConnectedText, current.code, current.voice, dummyTiming);
 Serial.printf("\n>>> System Ready! Current Language: %s <<<\n", current.name);
 Serial.println(">>> Capture Photo: GPIO 13 | Language Select: GPIO 2 <<<");
}
void loop() {
 if (millis() - lastBtnPressTime < 500) return;
 // Button 13: Capture Image & Process
 if (digitalRead(CAPTURE_BTN_PIN) == LOW) {
   lastBtnPressTime = millis();
   captureAndAnalyze();
 }
 // Button 2: Toggle Language
 if (digitalRead(LANG_BTN_PIN) == LOW) {
   lastBtnPressTime = millis();
   currentLangIndex = (currentLangIndex + 1) % TOTAL_LANGUAGES;
   LanguageConfig current = languages[currentLangIndex];
   Serial.printf("\n[LANG] Switched to: %s\n", current.name);
   TTSTiming dummyTiming = {0};
   speakWithSarvam(current.notifyText, current.code, current.voice, dummyTiming);
 }
}
void captureAndAnalyze() {
 unsigned long t_total_start = millis();
 unsigned long t_cam_init = 0;
 unsigned long t_cam_snap = 0;
 unsigned long t_cam_deinit = 0;
 unsigned long t_ssl_connect = 0;
 unsigned long t_img_upload = 0;
 unsigned long t_vision_proc = 0;
 unsigned long t_json_parse = 0;
 TTSTiming tts = {0};
 LanguageConfig current = languages[currentLangIndex];
 Serial.println("\n==================================================");
 Serial.printf("   STARTING IMAGE ANALYSIS (%s Mode)\n", current.name);
 Serial.println("==================================================");
 // 1. Initialize Camera Hardware
 unsigned long t0 = millis();
 if (!initCameraHardware()) {
   Serial.println("[CAMERA ERROR] Failed hardware initialization!");
   return;
 }
 t_cam_init = millis() - t0;
 // 2. Snap Frame (raw RGB565)
 digitalWrite(FLASH_LED_PIN, HIGH);
 t0 = millis();
 camera_fb_t * fb = esp_camera_fb_get();
 digitalWrite(FLASH_LED_PIN, LOW);
 t_cam_snap = millis() - t0;
 if (!fb) {
   Serial.println("[CAMERA ERROR] Frame buffer capture failed!");
   esp_camera_deinit();
   return;
 }
 // 2b. NEW: Convert raw RGB565 frame to JPEG in software.
 // This sensor has no hardware JPEG encoder, so PIXFORMAT_JPEG at
 // esp_camera_init() fails with "JPEG format is not supported on this
 // sensor". frame2jpg() encodes JPEG on the CPU from any raw format.
 t0 = millis();
 uint8_t * jpg_buf = NULL;
 size_t jpg_len = 0;
 bool jpeg_ok = frame2jpg(fb, 80, &jpg_buf, &jpg_len); // 80 = JPEG quality (0-100)
 unsigned long t_jpeg_convert = millis() - t0;
 esp_camera_fb_return(fb); // raw frame no longer needed once converted
 if (!jpeg_ok || !jpg_buf) {
   Serial.println("[CAMERA ERROR] RGB565->JPEG conversion failed!");
   esp_camera_deinit();
   return;
 }
 // 3. Immediately De-initialize Camera to free GPIO 12, 14, 15
 t0 = millis();
 esp_camera_deinit(); 
 t_cam_deinit = millis() - t0;
 // 4. Connect SSL Client
 t0 = millis();
 WiFiClientSecure client;
 client.setInsecure();
 client.setTimeout(10000);
 const char* host = "www.circuitdigest.cloud";
 if (!client.connect(host, 443)) {
   Serial.println("[CLOUD ERROR] Could not connect to Vision Cloud!");
   free(jpg_buf);
   return;
 }
 t_ssl_connect = millis() - t0;
 // 5. Construct Multipart Upload Header
 String boundary = "----ESP32Boundary12345";
 String headPrompt = "--" + boundary + "\r\n" +
                     "Content-Disposition: form-data; name=\"prompt\"\r\n\r\n" +
                     String(current.prompt) + "\r\n";
 String headImage = "--" + boundary + "\r\n" +
                    "Content-Disposition: form-data; name=\"imageFile\"; filename=\"photo.jpg\"\r\n" +
                    "Content-Type: image/jpeg\r\n\r\n";
 String tail = "\r\n--" + boundary + "--\r\n";
 size_t contentLength = headPrompt.length() + headImage.length() + jpg_len + tail.length();
 // Stream POST Request Header & Body Chunks
 t0 = millis();
 client.println("POST /api/v1/image-to-text/generate HTTP/1.1");
 client.println("Host: www.circuitdigest.cloud");
 client.println("X-API-Key: " + String(visionApiKey));
 client.println("Content-Type: multipart/form-data; boundary=" + boundary);
 client.printf("Content-Length: %d\r\n", contentLength);
 client.println("Connection: close");
 client.println();
 client.print(headPrompt);
 client.print(headImage);
 uint8_t *fbBuf = jpg_buf;
 size_t fbLen = jpg_len;
 size_t chunkSize = 1024;
 for (size_t i = 0; i < fbLen; i += chunkSize) {
   size_t len = (fbLen - i < chunkSize) ? (fbLen - i) : chunkSize;
   client.write(fbBuf + i, len);
 }
 client.print(tail);
 client.flush();
 t_img_upload = millis() - t0;
 free(jpg_buf); // done with the encoded JPEG buffer
 // 6. Fast Response Parsing directly from Socket Stream (No 15-second hang!)
 t0 = millis();
 while (client.connected() && !client.available()) {
   if (millis() - t0 > 10000) {
     Serial.println("[CLOUD ERROR] Response Timeout!");
     client.stop();
     return;
   }
   delay(5);
 }
 // Skip HTTP Headers efficiently
 while (client.connected()) {
   String line = client.readStringUntil('\n');
   if (line == "\r" || line.length() == 0) break;
 }
 t_vision_proc = millis() - t0;
 // 7. Directly Deserialize JSON Stream
 t0 = millis();
 JsonDocument doc;
 DeserializationError error = deserializeJson(doc, client);
 client.stop();
 t_json_parse = millis() - t0;
 if (!error) {
   String generatedText = "";
   if (doc.containsKey("generated_text")) {
     generatedText = doc["generated_text"].as<String>();
   } else if (doc.containsKey("text")) {
     generatedText = doc["text"].as<String>();
   }
   generatedText.replace("\"", "");
   generatedText.replace("\'", "");
   generatedText.replace("\\", "");
   generatedText.replace("\r", "");
   generatedText.replace("\n", " ");
   generatedText.trim();
   if (generatedText.length() > 0) {
     Serial.printf("\n[VISION RESULT (%s)] %s\n", current.name, generatedText.c_str());
     // 8. Execute Sarvam Text-to-Speech
     speakWithSarvam(generatedText, current.code, current.voice, tts);
     unsigned long t_total_elapsed = millis() - t_total_start;
     // Print Accurate Speed Benchmarks
     Serial.println("\n--------------------------------------------------");
     Serial.println("          REAL-TIME SPEED BENCHMARK               ");
     Serial.println("--------------------------------------------------");
     Serial.printf("  1. Camera Hardware Init : %lu ms\n", t_cam_init);
     Serial.printf("  2. Image Snap Frame     : %lu ms\n", t_cam_snap);
     Serial.printf("  2b. RGB565->JPEG Convert: %lu ms\n", t_jpeg_convert);
     Serial.printf("  3. Camera Deinit        : %lu ms\n", t_cam_deinit);
     Serial.printf("  4. Vision SSL Connect   : %lu ms\n", t_ssl_connect);
     Serial.printf("  5. Image Upload Stream  : %lu ms\n", t_img_upload);
     Serial.printf("  6. Vision AI Response   : %lu ms (FAST FIX!)\n", t_vision_proc);
     Serial.printf("  7. Vision JSON Parse    : %lu ms\n", t_json_parse);
     Serial.println("--------------------------------------------------");
     Serial.printf("  8. Sarvam SSL Connect   : %lu ms\n", tts.tts_connect);
     Serial.printf("  9. Sarvam POST Request  : %lu ms\n", tts.tts_post);
     Serial.printf(" 10. Download Audio B64   : %lu ms\n", tts.tts_download);
     Serial.printf(" 11. Extract Base64 Str   : %lu ms\n", tts.tts_extract);
     Serial.printf(" 12. Base64 Audio Decode  : %lu ms\n", tts.tts_decode);
     Serial.printf(" 13. Speaker Audio Output : %lu ms\n", tts.tts_playback);
     Serial.println("--------------------------------------------------");
     Serial.printf(" TOTAL END-TO-END DELAY   : %lu ms (%.2f sec)\n", t_total_elapsed, t_total_elapsed / 1000.0f);
     Serial.println("--------------------------------------------------\n");
   }
 } else {
   Serial.print("[JSON ERROR] Deserialization failed: ");
   Serial.println(error.c_str());
 }
}
