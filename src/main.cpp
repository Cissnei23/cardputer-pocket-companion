#include <Arduino.h>
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <time.h>
#include "pocket_claude_secrets.h"
#include "pocket_claude_config.h"

#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutput.h>

namespace {
constexpr int kSdSck = 40;
constexpr int kSdMiso = 39;
constexpr int kSdMosi = 14;
constexpr int kSdCs = 12;
constexpr size_t kMaxSongs = 32;
constexpr size_t kBufferCount = 3;
constexpr size_t kBufferSize = 2048;
String bridgeUrl(const char* endpoint) {
  return String(kPocketClaudeBridgeBaseUrl) + endpoint;
}
constexpr size_t kMaxConversations = 64;

enum class Screen { ClaudeHome, Chat, Conversations, Library, Player, Finished };
enum class PlayMode { Once, RepeatOne, Sequential };
enum class PlaybackResult { Finished, Back, Error };

String songPaths[kMaxSongs];
String songNames[kMaxSongs];
size_t songCount = 0;
size_t selectedSong = 0;
Screen screen = Screen::ClaudeHome;
PlayMode playMode = PlayMode::Once;
bool paused = false;
int volume = 64;
uint8_t audioBuffers[kBufferCount][kBufferSize];
bool wifiConnected = false;
bool bridgeAwake = false;
uint32_t lastBridgeCheck = 0;
bool sdReady = false;
int fiveHourRemaining = -1;
int weekRemaining = -1;
String fiveHourReset;
String weekReset;
uint32_t lastUsageCheck = 0;
int currentHour = -1;
int greetingPeriod = -1;
String greetingText = "there you are.";
String chatInput;
String chatReply = "ask me something.";
uint8_t chatModel = 0;
const char* chatModelKeys[] = {"haiku", "sonnet", "opus"};
const char* chatModelLabels[] = {"HAIKU 4.5", "SONNET 4.5", "OPUS 4.5"};

struct Conversation {
  String id;
  String name;
  String createdAt;
};
Conversation convList[kMaxConversations];
size_t convCount = 0;
size_t convSelected = 0;
String currentConvId;
bool convDeleteConfirm = false;

struct __attribute__((packed)) WavHeader {
  char riff[4];
  uint32_t chunkSize;
  char waveFmt[8];
  uint32_t fmtChunkSize;
  uint16_t audioFormat;
  uint16_t channels;
  uint32_t sampleRate;
  uint32_t bytesPerSecond;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
};

struct __attribute__((packed)) SubChunk {
  char id[4];
  uint32_t size;
};

class M5SpeakerOutput : public AudioOutput {
 public:
  explicit M5SpeakerOutput(m5::Speaker_Class* speaker) : speaker_(speaker) {}
  bool begin() override { return true; }

  bool ConsumeSample(int16_t sample[2]) override {
    if (index_ + 1 < kMp3BufferSize) {
      buffers_[buffer_][index_++] = sample[0];
      buffers_[buffer_][index_++] = sample[1];
      return true;
    }
    flush();
    return false;
  }

  void flush() override {
    if (!index_) return;
    speaker_->playRaw(buffers_[buffer_], index_, hertz, true, 1, 0);
    buffer_ = (buffer_ + 1) % 3;
    index_ = 0;
  }

  bool stop() override {
    flush();
    speaker_->stop(0);
    return true;
  }

 private:
  static constexpr size_t kMp3BufferSize = 1536;
  m5::Speaker_Class* speaker_;
  int16_t buffers_[3][kMp3BufferSize]{};
  size_t buffer_ = 0;
  size_t index_ = 0;
};

AudioFileSourceSD mp3Source;
AudioGeneratorMP3 mp3Decoder;
M5SpeakerOutput mp3Output(&M5Cardputer.Speaker);

uint16_t warmBlack() { return M5Cardputer.Display.color565(23, 19, 16); }
uint16_t clayOrange() { return M5Cardputer.Display.color565(217, 119, 87); }
uint16_t warmWhite() { return M5Cardputer.Display.color565(242, 232, 220); }
uint16_t mutedBrown() { return M5Cardputer.Display.color565(169, 154, 141); }
uint16_t darkBrown() { return M5Cardputer.Display.color565(70, 53, 44); }
uint16_t sageGreen() { return M5Cardputer.Display.color565(143, 166, 122); }

void drawUsageBar(int y, const char* label, int remaining, const String& reset) {
  auto& d = M5Cardputer.Display;
  d.setTextSize(1);
  d.setTextColor(warmWhite(), warmBlack());
  d.setCursor(10, y);
  d.print(label);
  d.drawRoundRect(45, y - 2, 95, 11, 3, darkBrown());
  if (remaining >= 0) {
    uint16_t color = remaining < 20 ? M5Cardputer.Display.color565(166, 71, 60)
                                    : (remaining < 60 ? M5Cardputer.Display.color565(196, 145, 74)
                                                      : clayOrange());
    int width = map(constrain(remaining, 0, 100), 0, 100, 0, 91);
    if (width > 0) d.fillRoundRect(47, y, width, 7, 2, color);
    d.setTextColor(warmWhite(), warmBlack());
    d.setCursor(147, y);
    d.printf("%d%%", remaining);
    d.setTextColor(mutedBrown(), warmBlack());
    d.setCursor(190, y);
    d.print(reset);
  } else {
    d.setTextColor(mutedBrown(), warmBlack());
    d.setCursor(150, y);
    d.print("syncing...");
  }
}

int periodForHour(int hour) {
  if (hour < 5) return 0;
  if (hour < 9) return 1;
  if (hour < 12) return 2;
  if (hour < 14) return 3;
  if (hour < 18) return 4;
  if (hour < 22) return 5;
  return 6;
}

void chooseTimeGreeting(bool force = false) {
  if (currentHour < 0) return;
  int period = periodForHour(currentHour);
  if (!force && period == greetingPeriod) return;
  static const char* greetings[][3] = {
      {"...still awake?", "we should sleep.", "just one more?"},
      {"you're up early.", "morning, sleepy.", "already awake?"},
      {"good morning.", "morning, you.", "what's the plan?"},
      {"lunch first.", "have you eaten?", "food, then code."},
      {"good afternoon.", "there you are.", "found anything?"},
      {"welcome back.", "how was your day?", "you're home."},
      {"it's getting late.", "one last question?", "still not sleepy?"},
  };
  greetingPeriod = period;
  greetingText = greetings[period][esp_random() % 3];
}

void drawWifiSetup() {
  auto& d = M5Cardputer.Display;
  d.fillScreen(warmBlack());
  d.setTextColor(clayOrange(), warmBlack());
  d.setTextSize(2);
  d.setCursor(10, 12);
  d.print("wifi setup");
  d.setTextSize(1);
  d.setTextColor(warmWhite(), warmBlack());
  d.setCursor(10, 44);
  d.println("1. connect your phone to:");
  d.setTextColor(clayOrange(), warmBlack());
  d.setCursor(18, 61);
  d.println("PocketClaude-Setup");
  d.setTextColor(warmWhite(), warmBlack());
  d.setCursor(10, 82);
  d.println("2. choose your home wifi");
  d.setCursor(10, 98);
  d.println("3. enter its password");
  d.setTextColor(mutedBrown(), warmBlack());
  d.setCursor(10, 119);
  d.print("waiting for you...");
}

bool fetchConversations() {
  if (!bridgeAwake || WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(5000);
  if (!http.begin(bridgeUrl("/conversations"))) return false;
  http.addHeader("Authorization", String("Bearer ") + kPocketClaudeToken);
  int status = http.GET();
  String response = http.getString();
  http.end();
  if (status != HTTP_CODE_OK) return false;
  DynamicJsonDocument doc(16384);
  if (deserializeJson(doc, response)) return false;
  JsonArray arr = doc["conversations"].as<JsonArray>();
  size_t count = 0;
  for (JsonObject obj : arr) {
    if (count >= kMaxConversations) break;
    convList[count].id = obj["id"].as<String>();
    convList[count].name = obj["name"].as<String>();
    convList[count].createdAt = obj["createdAt"].as<String>();
    ++count;
  }
  convCount = count;
  if (convSelected >= convCount && convCount > 0) convSelected = convCount - 1;
  return true;
}

bool deleteConversationRemote(const String& id) {
  if (!bridgeAwake || WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(5000);
  String url = bridgeUrl("/conversations/") + id;
  if (!http.begin(url)) return false;
  http.addHeader("Authorization", String("Bearer ") + kPocketClaudeToken);
  int status = http.sendRequest("DELETE");
  http.end();
  return status == HTTP_CODE_OK;
}

bool checkBridge() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setConnectTimeout(1200);
  http.setTimeout(1800);
  if (!http.begin(bridgeUrl("/health"))) return false;
  int status = http.GET();
  String response = http.getString();
  http.end();
  if (status != HTTP_CODE_OK) return false;
  StaticJsonDocument<256> document;
  if (!deserializeJson(document, response) && document["hour"].is<int>()) {
    currentHour = document["hour"].as<int>();
    chooseTimeGreeting();
  }
  return true;
}

bool updateUsage();

void refreshClaudeConnection() {
  wifiConnected = WiFi.status() == WL_CONNECTED;
  if (!wifiConnected) {
    bridgeAwake = false;
    return;
  }
  bridgeAwake = checkBridge();
  lastBridgeCheck = millis();
  if (bridgeAwake) updateUsage();
}

bool ensureWifiForClaude() {
  refreshClaudeConnection();
  if (wifiConnected) return true;

  drawWifiSetup();
  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(180);
  wifiManager.setConnectTimeout(20);
  wifiConnected = wifiManager.autoConnect("PocketClaude-Setup");
  if (wifiConnected) {
    configTime(8 * 3600, 0, "pool.ntp.org", "time.cloudflare.com");
    struct tm syncedTime{};
    getLocalTime(&syncedTime, 5000);
    refreshClaudeConnection();
  } else {
    bridgeAwake = false;
  }
  return wifiConnected;
}

bool updateUsage() {
  if (!bridgeAwake || WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setConnectTimeout(1500);
  http.setTimeout(4000);
  if (!http.begin(bridgeUrl("/usage"))) return false;
  http.addHeader("Authorization", String("Bearer ") + kPocketClaudeToken);
  int status = http.GET();
  String response = http.getString();
  http.end();
  if (status != HTTP_CODE_OK) return false;
  StaticJsonDocument<768> document;
  if (deserializeJson(document, response)) return false;
  fiveHourRemaining = round(document["fiveHourRemaining"].as<float>());
  weekRemaining = round(document["weekRemaining"].as<float>());
  fiveHourReset = document["fiveHourReset"].as<String>();
  weekReset = document["weekReset"].as<String>();
  lastUsageCheck = millis();
  return true;
}

void drawClaudeHome() {
  auto& d = M5Cardputer.Display;
  const uint16_t bg = warmBlack();
  d.fillScreen(bg);
  d.setTextSize(1);
  d.setTextColor(clayOrange(), bg);
  d.setCursor(8, 7);
  d.print("claude");
  d.setTextColor(mutedBrown(), bg);
  d.setCursor(172, 7);
  d.print(chatModelLabels[chatModel]);
  d.drawFastHLine(8, 20, 224, darkBrown());

  d.fillCircle(11, 34, 3, bridgeAwake ? sageGreen() : darkBrown());
  d.setTextColor(warmWhite(), bg);
  d.setCursor(20, 31);
  if (!wifiConnected) d.print("wifi offline");
  else if (bridgeAwake) d.print("awake");
  else d.print("sleeping at home... zZz");
  d.setTextColor(clayOrange(), bg);
  d.setTextSize(2);
  d.setCursor(10, 47);
  d.print(greetingText);

  drawUsageBar(76, "5H", fiveHourRemaining, fiveHourReset);
  drawUsageBar(96, "WEEK", weekRemaining, weekReset);
  d.setTextColor(mutedBrown(), bg);
  d.setTextSize(1);
  d.setCursor(8, 121);
  d.print("[C] chat          [M] music");
}

void printWrapped(const String& text, int x, int y, int charsPerLine, int maxLines) {
  auto& d = M5Cardputer.Display;
  String clean = text;
  clean.replace("\r", " ");
  clean.replace("\n", " ");
  while (clean.indexOf("  ") >= 0) clean.replace("  ", " ");
  int start = 0;
  for (int line = 0; line < maxLines && start < static_cast<int>(clean.length()); ++line) {
    int count = min(charsPerLine, static_cast<int>(clean.length()) - start);
    int end = start + count;
    if (end < static_cast<int>(clean.length())) {
      int space = clean.lastIndexOf(' ', end);
      if (space > start + charsPerLine / 2) end = space;
    }
    d.setCursor(x, y + line * 12);
    d.print(clean.substring(start, end));
    start = end;
    while (start < static_cast<int>(clean.length()) && clean[start] == ' ') ++start;
  }
}

void drawChat(const char* status = nullptr) {
  auto& d = M5Cardputer.Display;
  const uint16_t bg = warmBlack();
  d.fillScreen(bg);
  d.setTextSize(1);
  d.setTextColor(clayOrange(), bg);
  d.setCursor(8, 7);
  d.print("pocket chat");
  d.setTextColor(mutedBrown(), bg);
  d.setCursor(172, 7);
  d.print(chatModelLabels[chatModel]);
  d.drawFastHLine(8, 20, 224, darkBrown());

  d.setTextColor(status ? clayOrange() : warmWhite(), bg);
  printWrapped(status ? String(status) : chatReply, 9, 29, 36, 6);

  d.drawFastHLine(8, 104, 224, darkBrown());
  d.setTextColor(clayOrange(), bg);
  d.setCursor(8, 110);
  d.print("> ");
  d.setTextColor(warmWhite(), bg);
  String visible = chatInput.length() > 29 ? chatInput.substring(chatInput.length() - 29) : chatInput;
  d.print(visible);
  d.setTextColor(mutedBrown(), bg);
  d.setCursor(8, 124);
  d.print("TAB model  ENTER send  FN+B back");
}

void drawConversations() {
  auto& d = M5Cardputer.Display;
  const uint16_t bg = warmBlack();
  d.fillScreen(bg);
  d.setTextSize(1);
  d.setTextColor(clayOrange(), bg);
  d.setCursor(8, 7);
  d.print("chats");
  d.drawFastHLine(8, 20, 224, darkBrown());

  if (convCount == 0) {
    d.setTextColor(mutedBrown(), bg);
    d.setCursor(8, 50);
    d.print("no conversations yet.");
    d.setCursor(8, 64);
    d.print("press N to start one.");
  } else {
    size_t first = convSelected > 2 ? convSelected - 2 : 0;
    if (first + 5 > convCount && convCount > 5) first = convCount - 5;
    for (size_t i = first; i < convCount && i < first + 5; ++i) {
      int y = 29 + static_cast<int>(i - first) * 18;
      bool sel = (i == convSelected);
      if (sel) {
        d.fillRoundRect(5, y - 3, 230, 16, 2, darkBrown());
        d.setTextColor(clayOrange(), darkBrown());
      } else {
        d.setTextColor(warmWhite(), bg);
      }
      d.setCursor(9, y);
      String label = convList[i].name;
      if (label.isEmpty() && convList[i].createdAt.length() >= 16) {
        label = convList[i].createdAt.substring(5, 10) + " " +
                convList[i].createdAt.substring(11, 16);
      }
      d.print(label.substring(0, 30));
    }
  }

  if (convDeleteConfirm) {
    d.fillRect(5, 100, 230, 34, darkBrown());
    d.setTextColor(clayOrange(), darkBrown());
    d.setCursor(9, 104);
    d.print("delete this chat?");
    d.setTextColor(warmWhite(), darkBrown());
    d.setCursor(9, 116);
    d.print("ENTER yes   any key cancel");
  } else {
    d.setTextColor(mutedBrown(), bg);
    d.setCursor(8, 124);
    if (convCount > 0)
      d.print("W/S  ENTER open  N new  D del  FN+B");
    else
      d.print("N new chat   FN+B back");
  }
}

void sendChat() {
  if (!bridgeAwake || chatInput.isEmpty()) {
    chatReply = bridgeAwake ? "type something first." : "claude is sleeping at home.";
    drawChat();
    return;
  }
  String question = chatInput;
  chatInput = "";
  drawChat("thinking...");

  StaticJsonDocument<1024> request;
  request["model"] = chatModelKeys[chatModel];
  request["message"] = question;
  if (!currentConvId.isEmpty()) request["conversationId"] = currentConvId;
  String payload;
  serializeJson(request, payload);

  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(60000);
  if (!http.begin(bridgeUrl("/chat"))) {
    chatReply = "i couldn't reach home.";
    drawChat();
    return;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + kPocketClaudeToken);
  int status = http.POST(payload);
  String response = http.getString();
  http.end();
  if (status != HTTP_CODE_OK) {
    chatReply = "something went wrong at home.";
    drawChat();
    return;
  }
  DynamicJsonDocument document(8192);
  if (deserializeJson(document, response)) {
    chatReply = "i couldn't read the reply.";
  } else {
    chatReply = document["reply"].as<String>();
    if (currentConvId.isEmpty()) {
      String newId = document["conversationId"].as<String>();
      if (!newId.isEmpty() && newId != "null") currentConvId = newId;
    }
  }
  if (chatReply.isEmpty()) chatReply = "...";
  drawChat();
}

const char* modeName() {
  switch (playMode) {
    case PlayMode::RepeatOne: return "Repeat one";
    case PlayMode::Sequential: return "Sequential";
    default: return "Play once";
  }
}

bool isSupportedAudio(String value) {
  value.toLowerCase();
  return value.endsWith(".wav") || value.endsWith(".mp3");
}

void scanSongs() {
  songCount = 0;
  File directory = SD.open("/music");
  if (!directory || !directory.isDirectory()) return;

  File entry = directory.openNextFile();
  while (entry && songCount < kMaxSongs) {
    if (!entry.isDirectory()) {
      String path = entry.path();
      if (isSupportedAudio(path)) {
        songPaths[songCount] = path;
        String name = path.substring(path.lastIndexOf('/') + 1);
        name.remove(name.length() - 4);
        String lowerPath = path;
        lowerPath.toLowerCase();
        name += lowerPath.endsWith(".mp3") ? " [MP3]" : " [WAV]";
        songNames[songCount] = name;
        ++songCount;
      }
    }
    entry.close();
    entry = directory.openNextFile();
  }
  directory.close();
  if (selectedSong >= songCount) selectedSong = 0;
}

void drawLibrary() {
  auto& d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextColor(TFT_CYAN, TFT_BLACK);
  d.setTextSize(2);
  d.setCursor(8, 6);
  d.println("MUSIC LIBRARY");
  d.drawFastHLine(8, 29, 224, TFT_DARKGREY);
  d.setTextSize(1);

  if (!songCount) {
    d.setTextColor(TFT_YELLOW, TFT_BLACK);
    d.setCursor(8, 48);
    d.println("No songs in /music");
    return;
  }

  size_t first = selectedSong > 2 ? selectedSong - 2 : 0;
  if (first + 5 > songCount && songCount > 5) first = songCount - 5;
  for (size_t i = first; i < songCount && i < first + 5; ++i) {
    int y = 38 + static_cast<int>(i - first) * 17;
    if (i == selectedSong) {
      d.fillRoundRect(5, y - 2, 230, 15, 3, TFT_DARKCYAN);
      d.setTextColor(TFT_WHITE, TFT_DARKCYAN);
      d.setCursor(9, y);
      d.print("> ");
    } else {
      d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      d.setCursor(9, y);
      d.print("  ");
    }
    d.print(songNames[i].substring(0, 30));
  }
  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  d.setCursor(8, 122);
  d.print("W/S select Enter play C claude");
}

void drawPlayer(const char* status) {
  auto& d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextColor(TFT_CYAN, TFT_BLACK);
  d.setTextSize(2);
  d.setCursor(8, 6);
  d.println("POCKET PLAYER");
  d.setTextSize(1);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setCursor(8, 38);
  d.println(songCount ? songNames[selectedSong].substring(0, 34) : "No song");
  d.setTextColor(TFT_YELLOW, TFT_BLACK);
  d.setCursor(8, 62);
  d.printf("Status: %s\n", status);
  d.printf("Mode:   %s\n", modeName());
  d.printf("Volume: %d\n", volume);
  d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  d.setCursor(8, 112);
  d.print("P pause M mode B back -/+");
}

void drawFinished() {
  drawPlayer("FINISHED");
  auto& d = M5Cardputer.Display;
  d.fillRect(5, 105, 230, 28, TFT_BLACK);
  d.setTextColor(TFT_GREEN, TFT_BLACK);
  d.setCursor(8, 108);
  d.println("R replay");
  d.println("B back to library");
}

void nextMode() {
  if (playMode == PlayMode::Once) playMode = PlayMode::RepeatOne;
  else if (playMode == PlayMode::RepeatOne) playMode = PlayMode::Sequential;
  else playMode = PlayMode::Once;
}

bool keyPressed(char key) {
  return M5Cardputer.Keyboard.isKeyPressed(key);
}

bool pollKeyboard() {
  M5Cardputer.update();
  return M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed();
}

PlaybackResult playWav(const String& path) {
  File file = SD.open(path, FILE_READ);
  if (!file) {
    drawPlayer("FILE NOT FOUND");
    delay(1200);
    return PlaybackResult::Error;
  }

  WavHeader header{};
  if (file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header) ||
      memcmp(header.riff, "RIFF", 4) || memcmp(header.waveFmt, "WAVEfmt ", 8) ||
      header.audioFormat != 1 || header.bitsPerSample != 16 ||
      header.channels == 0 || header.channels > 2) {
    file.close();
    drawPlayer("BAD WAV FILE");
    delay(1200);
    return PlaybackResult::Error;
  }

  file.seek(offsetof(WavHeader, audioFormat) + header.fmtChunkSize);
  SubChunk chunk{};
  while (file.read(reinterpret_cast<uint8_t*>(&chunk), sizeof(chunk)) == sizeof(chunk)) {
    if (!memcmp(chunk.id, "data", 4)) break;
    if (!file.seek(chunk.size + (chunk.size & 1), SeekMode::SeekCur)) break;
  }
  if (memcmp(chunk.id, "data", 4)) {
    file.close();
    drawPlayer("NO AUDIO DATA");
    delay(1200);
    return PlaybackResult::Error;
  }

  paused = false;
  drawPlayer("PLAYING");
  uint32_t remaining = chunk.size;
  size_t bufferIndex = 0;
  while (remaining > 0) {
    if (pollKeyboard()) {
      if (keyPressed('p')) {
        paused = !paused;
        drawPlayer(paused ? "PAUSED" : "PLAYING");
      } else if (keyPressed('b')) {
        M5Cardputer.Speaker.stop(0);
        file.close();
        return PlaybackResult::Back;
      } else if (keyPressed('m')) {
        nextMode();
        drawPlayer(paused ? "PAUSED" : "PLAYING");
      } else if (keyPressed('-')) {
        volume = max(8, volume - 8);
        M5Cardputer.Speaker.setVolume(volume);
        drawPlayer(paused ? "PAUSED" : "PLAYING");
      } else if (keyPressed('=') || keyPressed('+')) {
        volume = min(128, volume + 8);
        M5Cardputer.Speaker.setVolume(volume);
        drawPlayer(paused ? "PAUSED" : "PLAYING");
      }
    }
    if (paused) {
      delay(10);
      continue;
    }

    size_t wanted = min(static_cast<uint32_t>(kBufferSize), remaining);
    size_t count = file.read(audioBuffers[bufferIndex], wanted);
    if (!count) break;
    remaining -= count;
    M5Cardputer.Speaker.playRaw(
        reinterpret_cast<const int16_t*>(audioBuffers[bufferIndex]), count / 2,
        header.sampleRate, header.channels > 1, 1, 0);
    bufferIndex = (bufferIndex + 1) % kBufferCount;
  }
  file.close();
  while (M5Cardputer.Speaker.isPlaying(0)) delay(5);
  return PlaybackResult::Finished;
}

PlaybackResult playMp3(const String& path) {
  if (!mp3Source.open(path.c_str())) {
    drawPlayer("FILE NOT FOUND");
    delay(1200);
    return PlaybackResult::Error;
  }
  if (!mp3Decoder.begin(&mp3Source, &mp3Output)) {
    mp3Source.close();
    drawPlayer("MP3 START ERROR");
    delay(1200);
    return PlaybackResult::Error;
  }

  paused = false;
  drawPlayer("PLAYING");
  while (mp3Decoder.isRunning()) {
    if (pollKeyboard()) {
      if (keyPressed('p')) {
        paused = !paused;
        drawPlayer(paused ? "PAUSED" : "PLAYING");
      } else if (keyPressed('b')) {
        mp3Decoder.stop();
        mp3Source.close();
        return PlaybackResult::Back;
      } else if (keyPressed('m')) {
        nextMode();
        drawPlayer(paused ? "PAUSED" : "PLAYING");
      } else if (keyPressed('-')) {
        volume = max(8, volume - 8);
        M5Cardputer.Speaker.setVolume(volume);
        drawPlayer(paused ? "PAUSED" : "PLAYING");
      } else if (keyPressed('=') || keyPressed('+')) {
        volume = min(128, volume + 8);
        M5Cardputer.Speaker.setVolume(volume);
        drawPlayer(paused ? "PAUSED" : "PLAYING");
      }
    }
    if (paused) {
      delay(10);
      continue;
    }
    if (!mp3Decoder.loop()) break;
  }
  mp3Decoder.stop();
  mp3Source.close();
  return PlaybackResult::Finished;
}

void startSelectedSong() {
  screen = Screen::Player;
  String path = songPaths[selectedSong];
  String lowerPath = path;
  lowerPath.toLowerCase();
  PlaybackResult result = lowerPath.endsWith(".mp3") ? playMp3(path) : playWav(path);
  if (result == PlaybackResult::Back || result == PlaybackResult::Error) {
    screen = Screen::Library;
    drawLibrary();
    return;
  }

  if (playMode == PlayMode::RepeatOne) return;
  if (playMode == PlayMode::Sequential && selectedSong + 1 < songCount) {
    ++selectedSong;
    return;
  }
  screen = Screen::Finished;
  drawFinished();
}
}  // namespace

void setup() {
  auto config = M5.config();
  M5Cardputer.begin(config, true);
  M5Cardputer.Display.setRotation(1);

  auto speakerConfig = M5Cardputer.Speaker.config();
  speakerConfig.sample_rate = 96000;
  M5Cardputer.Speaker.config(speakerConfig);
  M5Cardputer.Speaker.begin();
  M5Cardputer.Speaker.setVolume(volume);

  SPI.begin(kSdSck, kSdMiso, kSdMosi, kSdCs);
  sdReady = SD.begin(kSdCs, SPI, 25000000);
  if (sdReady) scanSongs();

  WiFi.mode(WIFI_STA);
  WiFi.begin();
  uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 3000) {
    delay(100);
  }
  wifiConnected = WiFi.status() == WL_CONNECTED;
  if (wifiConnected) {
    configTime(8 * 3600, 0, "pool.ntp.org", "time.cloudflare.com");
    struct tm syncedTime{};
    getLocalTime(&syncedTime, 5000);
    refreshClaudeConnection();
  }
  drawClaudeHome();
}

void loop() {
  if (screen == Screen::Player) {
    startSelectedSong();
    return;
  }

  if (screen == Screen::ClaudeHome && millis() - lastBridgeCheck > 10000) {
    bool wasConnected = wifiConnected;
    bool wasAwake = bridgeAwake;
    String oldGreeting = greetingText;
    wifiConnected = WiFi.status() == WL_CONNECTED;
    bridgeAwake = checkBridge();
    if (!wasAwake && bridgeAwake) greetingText = "...oh. you're here.";
    lastBridgeCheck = millis();
    bool usageChanged = false;
    if (bridgeAwake && (lastUsageCheck == 0 || millis() - lastUsageCheck > 60000)) {
      int oldFive = fiveHourRemaining;
      int oldWeek = weekRemaining;
      updateUsage();
      usageChanged = oldFive != fiveHourRemaining || oldWeek != weekRemaining;
    }
    if (wasConnected != wifiConnected || wasAwake != bridgeAwake || usageChanged ||
        oldGreeting != greetingText) drawClaudeHome();
  }

  if (!pollKeyboard()) {
    delay(10);
    return;
  }

  if (screen == Screen::ClaudeHome) {
    auto keys = M5Cardputer.Keyboard.keysState();
    if (keys.tab) {
      chatModel = (chatModel + 1) % 3;
      drawClaudeHome();
    } else if (keyPressed('c')) {
      if (!ensureWifiForClaude()) {
        drawClaudeHome();
        return;
      }
      fetchConversations();
      convSelected = 0;
      convDeleteConfirm = false;
      screen = Screen::Conversations;
      drawConversations();
    } else if (keyPressed('m')) {
      screen = Screen::Library;
      drawLibrary();
    }
  } else if (screen == Screen::Chat) {
    auto keys = M5Cardputer.Keyboard.keysState();
    bool back = keys.fn && (keyPressed('b') || keyPressed('B'));
    if (back) {
      fetchConversations();
      convDeleteConfirm = false;
      screen = Screen::Conversations;
      drawConversations();
    } else if (keys.tab || keys.opt || (keys.fn && (keyPressed('m') || keyPressed('M')))) {
      chatModel = (chatModel + 1) % 3;
      drawChat();
    } else if (keys.enter) {
      sendChat();
    } else {
      if (keys.del && !chatInput.isEmpty()) chatInput.remove(chatInput.length() - 1);
      for (char c : keys.word) {
        if (chatInput.length() < 180 && c >= 32 && c <= 126) chatInput += c;
      }
      drawChat();
    }
  } else if (screen == Screen::Conversations) {
    auto keys = M5Cardputer.Keyboard.keysState();
    bool back = keys.fn && (keyPressed('b') || keyPressed('B'));
    if (convDeleteConfirm) {
      if (keys.enter) {
        if (convSelected < convCount) {
          deleteConversationRemote(convList[convSelected].id);
          for (size_t i = convSelected; i + 1 < convCount; ++i) convList[i] = convList[i + 1];
          --convCount;
          if (convSelected >= convCount && convCount > 0) --convSelected;
        }
        convDeleteConfirm = false;
      } else {
        convDeleteConfirm = false;
      }
      drawConversations();
    } else if (back) {
      screen = Screen::ClaudeHome;
      drawClaudeHome();
    } else if (keyPressed('n') || keyPressed('N')) {
      currentConvId = "";
      chatInput = "";
      chatReply = "ask me something.";
      screen = Screen::Chat;
      drawChat();
    } else if (keys.enter && convCount > 0) {
      currentConvId = convList[convSelected].id;
      chatInput = "";
      chatReply = "continuing...";
      screen = Screen::Chat;
      drawChat();
    } else if ((keyPressed('d') || keyPressed('D')) && convCount > 0) {
      convDeleteConfirm = true;
      drawConversations();
    } else {
      bool moveUp = keyPressed('w') || keyPressed(',') || keyPressed(';');
      bool moveDown = keyPressed('s') || keyPressed('.') || keyPressed('/');
      if (moveUp && convSelected > 0) {
        --convSelected;
        drawConversations();
      } else if (moveDown && convSelected + 1 < convCount) {
        ++convSelected;
        drawConversations();
      }
    }
  } else if (screen == Screen::Library) {
    auto keys = M5Cardputer.Keyboard.keysState();
    // Cardputer prints arrows on the punctuation cluster. Depending on the
    // keyboard revision/modifier state, Up/Down can be reported as one of the
    // neighbouring punctuation characters, so accept the whole vertical pair.
    bool moveUp = keyPressed('w') || keyPressed(',') || keyPressed(';');
    bool moveDown = keyPressed('s') || keyPressed('.') || keyPressed('/');
    if (keyPressed('c')) {
      screen = Screen::ClaudeHome;
      drawClaudeHome();
    } else if (moveUp && selectedSong > 0) {
      --selectedSong;
      drawLibrary();
    } else if (moveDown && selectedSong + 1 < songCount) {
      ++selectedSong;
      drawLibrary();
    } else if (keys.enter && songCount) {
      screen = Screen::Player;
    }
  } else if (screen == Screen::Finished) {
    if (keyPressed('r')) {
      screen = Screen::Player;
    } else if (keyPressed('b')) {
      screen = Screen::Library;
      drawLibrary();
    } else if (keyPressed('m')) {
      nextMode();
      drawFinished();
    }
  }
}

