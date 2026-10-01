#include <LCDWIKI_GUI.h>
#include <LCDWIKI_KBV.h>
#include <DHT.h>
#include <SD.h>
#include <SPI.h>
#include <RTClib.h>
#include <Arduino_FreeRTOS.h>
#include <semphr.h>


LCDWIKI_KBV mylcd(ILI9481, 40, 38, 39, -1, 41);
// 放在全域
const char FILENAME[] = "temp.csv";


#define BLACK   0x0000
#define WHITE   0xFFFF
#define YELLOW  0xFFE0
#define CYAN    0x07FF
#define RED     0xF800

#define DHTPIN A0
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

const int chipSelect = 53;
const int MAX_RECORDS = 480;

int GRAPH_X = 85;
int GRAPH_Y = 80;
int GRAPH_W = 0;
int GRAPH_H = 160;
int GRAPH_BOTTOM = 0;

#define TEMP_MIN 22
#define TEMP_MAX 30
#define HUM_MIN 45
#define HUM_MAX 80

DateTime start_time;
unsigned long start_millis;
const char LAST_TIME_FILE[] = "lasttime.txt";  // 存啟動時間


#define BUTTON_PIN A2

volatile bool button_pressed = false;
bool screen_on = true;

struct GraphTick {
  int x;
  uint8_t hh;
  uint8_t mm;
};

struct Record {
  DateTime time;
  float temp;
  float hum;
};

SemaphoreHandle_t sdMutex;
SemaphoreHandle_t lcdMutex;


#define DISPLAY_TASK_SIZE 1024
#define GRAPH_POINT_MAX 400
#define TRIM_MARGIN 50
const char CSV_HEADER[] = "Timestamp,Temperature_C,Humidity_%";
const char TEMP_NAME[] = "temp.tmp";
const char TRIM_READY[] = "trim.rdy";

// 只留畫面上看得到的那段。畫線時不再握著 SD 鎖，也不用 String。
int16_t graphTemp10[GRAPH_POINT_MAX];
uint8_t graphHum[GRAPH_POINT_MAX];
volatile bool graphDirty = true;

static char sdBlock[128];

enum TimeAdjustMode { NONE, ADJUST_MINUTE, ADJUST_HOUR };
TimeAdjustMode adjustMode = NONE;

unsigned long adjustStartMillis = 0;
DateTime adjustTime;  // 暫存調整中的時間
volatile bool isAdjustingTime = false;

bool force_set_compile_time = false;


static bool takeSd(uint16_t timeoutMs) {
  return xSemaphoreTake(sdMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

static void giveSd() {
  xSemaphoreGive(sdMutex);
}

static bool takeLcd(uint16_t timeoutMs) {
  return xSemaphoreTake(lcdMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

static void giveLcd() {
  xSemaphoreGive(lcdMutex);
}

static int countAllLines(File& file) {
  int lines = 0;
  bool partial = false;
  for (;;) {
    int n = file.read((uint8_t*)sdBlock, (uint16_t)sizeof(sdBlock));
    if (n <= 0) break;
    for (int i = 0; i < n; i++) {
      partial = true;
      if (sdBlock[i] == '\n') {
        lines++;
        partial = false;
      }
    }
  }
  if (partial) lines++;
  return lines;
}

static int countDataLinesUnlocked() {
  File file = SD.open(FILENAME, FILE_READ);
  if (!file) return 0;
  int lines = countAllLines(file);
  file.close();
  if (lines <= 0) return 0;
  return lines - 1;
}

static File* lineFile = NULL;
static int lineLen = 0;
static int linePos = 0;
static char lineBuf[96];

static void lineReaderStart(File* file) {
  lineFile = file;
  lineLen = 0;
  linePos = 0;
}

static bool readCsvLine(char* out, size_t outSize) {
  if (outSize == 0 || lineFile == NULL) return false;
  size_t o = 0;
  bool any = false;
  for (;;) {
    if (linePos >= lineLen) {
      lineLen = lineFile->read((uint8_t*)lineBuf, (uint16_t)sizeof(lineBuf));
      linePos = 0;
      if (lineLen <= 0) {
        if (!any) return false;
        out[o] = '\0';
        return true;
      }
    }
    char c = lineBuf[linePos++];
    any = true;
    if (c == '\n') {
      if (o > 0 && out[o - 1] == '\r') o--;
      out[o] = '\0';
      return true;
    }
    if (o + 1 < outSize) out[o++] = c;
  }
}

static bool copyFile(File& src, File& dst) {
  for (;;) {
    int n = src.read((uint8_t*)sdBlock, (uint16_t)sizeof(sdBlock));
    if (n <= 0) return true;
    if (dst.write((const uint8_t*)sdBlock, (size_t)n) != (size_t)n) return false;
  }
}

static bool copyPathReplacing(const char* from, const char* to) {
  File src = SD.open(from, FILE_READ);
  if (!src) return false;
  SD.remove(to);
  File dst = SD.open(to, FILE_WRITE);
  if (!dst) {
    src.close();
    return false;
  }
  bool ok = copyFile(src, dst);
  src.close();
  dst.close();
  if (!ok) SD.remove(to);
  return ok;
}

// trim 寫到一半斷電時，trim.rdy 代表 temp.tmp 已是完整檔，開機用它還原。
static void recoverHistoryFile() {
  bool ready = SD.exists(TRIM_READY);
  bool hasTmp = SD.exists(TEMP_NAME);
  if (ready && hasTmp) {
    if (copyPathReplacing(TEMP_NAME, FILENAME)) {
      SD.remove(TEMP_NAME);
      SD.remove(TRIM_READY);
      Serial.println(F("[boot] 已從 temp.tmp 還原 temp.csv"));
    } else {
      Serial.println(F("[boot] temp.tmp 還原失敗"));
    }
    return;
  }
  if (hasTmp) SD.remove(TEMP_NAME);
  if (ready) SD.remove(TRIM_READY);
}

void setup() {
  Serial.begin(115200);
  // 初始化 SD 卡互斥鎖
  sdMutex = xSemaphoreCreateMutex();
  lcdMutex = xSemaphoreCreateMutex();

  
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  // attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);

  mylcd.Init_LCD();
  mylcd.Fill_Screen(BLACK);
  mylcd.Set_Rotation(1);

  int screen_w = mylcd.Get_Display_Width();
  GRAPH_W = screen_w - GRAPH_X - 20;
  GRAPH_BOTTOM = GRAPH_Y + GRAPH_H;

  dht.begin();
  pinMode(chipSelect, OUTPUT);

  if (!SD.begin(chipSelect)) {
    mylcd.Set_Text_colour(WHITE);
    mylcd.Set_Text_Size(2);
    mylcd.Print_String("SD Failed!", 10, 10);
    while (1);
  }

  recoverHistoryFile();

  // --------------- 時間初始化 -----------------
  compareAndSetStartTime();   // 取代原本的 loadLastTime + parseCompileTime
  // --------------------------------------------

  drawUI();

  // === 關鍵：開機自動補滿 480 筆假資料 ===
  ensureFullData();
//  drawGraphFromSD();
  // 建立任務（堆疊加大）
  xTaskCreate(TaskRecordSensor, "RecordSensor", 1024, NULL, 2, NULL);
  xTaskCreate(TaskUpdateDisplay, "UpdateDisplay", DISPLAY_TASK_SIZE, NULL, 1, NULL);
  // xTaskCreate(TaskSerialCommand, "SerialCmd", 1024, NULL, 1, NULL);
  xTaskCreate(TaskButtonHandler, "ButtonHandler", 1024, NULL, 1, NULL);  // 新增按鈕處理任務
}

void ensureFullData() {
  if (!takeSd(5000)) {
    Serial.println("[ERROR] 無法取得 SD mutex，跳過補資料");
    return;
  }

  int currentLines = countDataLinesUnlocked();

  if (currentLines >= MAX_RECORDS) {
    Serial.print("資料已足夠:");
    Serial.print(currentLines);
    Serial.println(" 筆，無需補充");
    giveSd();
    return;
  }

  Serial.print("資料不足（");
  Serial.print(currentLines);
  Serial.print(" 筆），開始補滿至 ");
  Serial.print(MAX_RECORDS);
  Serial.println(" 筆...");

  // 準備寫入（追加模式）
  File file = SD.open(FILENAME, FILE_WRITE);
  if (!file) {
    Serial.println("無法開啟 temp.csv");
    giveSd();
    return;
  }

  // 確保有 header
  if (file.size() == 0) {
    file.println(CSV_HEADER);
  } else {
    // 跳過 header，定位到最後
    file.seek(file.size());
  }

  // 計算要補多少筆
  int toFill = MAX_RECORDS - currentLines;

  // 從「現在時間」往前推
  DateTime now = getCurrentTime();
  DateTime baseTime = now - TimeSpan(0, currentLines + toFill - 1, 0, 0);

  for (int i = 0; i < toFill; i++) {
    DateTime t = baseTime + TimeSpan(0, i, 0, 0);

    // 假溫度：正弦波 + 噪聲
    float temp = 24.0 + 3.0 * sin((currentLines + i) * 0.13) + random(-80, 81) / 100.0;
    temp = constrain(temp, 22.0, 30.0);

    // 假濕度：週期變化
    int hum = 70 + 20 * sin((currentLines + i) * 0.08);
    hum = constrain(hum, 50, 100);

    char timestamp[20];
    sprintf(timestamp, "%04d-%02d-%02d %02d:%02d:00",
            t.year(), t.month(), t.day(), t.hour(), t.minute());

    file.print(timestamp);
    file.print(",");
    file.print(temp, 1);
    file.print(",");
    file.println(hum);
  }

  file.close();
  Serial.print("補充完成！總筆數：");
  Serial.println(countDataLinesUnlocked());
  giveSd();
}

/**
 * 比較 lasttime.txt 與編譯時間，選擇較晚的那個作為 start_time
 * 回傳 true  → 成功載入/設定
 * 回傳 false → 兩者都無法解析（極少發生），仍會用編譯時間
 */
bool compareAndSetStartTime() {
  DateTime compile_time = parseCompileTime();
  char compile_str[20];
  sprintf(compile_str, "%04d-%02d-%02d %02d:%02d:%02d",
          compile_time.year(), compile_time.month(), compile_time.day(),
          compile_time.hour(), compile_time.minute(), compile_time.second());
  // Serial.print(F("編譯時間: "));
  // Serial.println(compile_str);

  DateTime file_time(1970, 1, 1, 0, 0, 0);
  bool file_valid = false;

  File f = SD.open(LAST_TIME_FILE);
  if (f) {
    char file_str[20];
    size_t len = f.readBytesUntil('\n', file_str, sizeof(file_str) - 1);
    file_str[len] = '\0';
    f.close();

    int y, mo, d, h, mi, s;
    if (sscanf(file_str, "%04d-%02d-%02d %02d:%02d:%02d", &y, &mo, &d, &h, &mi, &s) == 6) {
      file_time = DateTime(y, mo, d, h, mi, s);
      file_valid = true;
      Serial.print(F("lasttime.txt 內容: "));
      Serial.println(file_str);
    }
  }

  if (!file_valid || compile_time >= file_time) {
    start_time = compile_time;
    // Serial.println(F("採用編譯時間"));
  } else {
    start_time = file_time;
    // Serial.println(F("採用檔案時間"));
  }

  if(force_set_compile_time == true)
    start_time = compile_time;

  // === 關鍵：先記錄 millis() ===
  start_millis = millis();

  // === 再寫入 SD ===
  updateLastTimeToSD(start_time);

  char final_str[20];
  sprintf(final_str, "%04d-%02d-%02d %02d:%02d:%02d",
          start_time.year(), start_time.month(), start_time.day(),
          start_time.hour(), start_time.minute(), start_time.second());
  Serial.print(F("最終採用時間: "));
  Serial.println(final_str);

  return true;
}

DateTime parseCompileTime() {
  const char* cd = __DATE__, *ct = __TIME__;
  char sm[5]; int y, mo, d, h, mi, s;
  sscanf(cd, "%s %d %d", sm, &d, &y);
  sscanf(ct, "%d:%d:%d", &h, &mi, &s);
  static const char month_names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  mo = (strstr(month_names, sm) - month_names) / 3 + 1;
  return DateTime(y, mo, d, h, mi, s);
}

bool loadLastTime() {
  File file = SD.open(LAST_TIME_FILE);
  if (!file) return false;

  char buf[20];
  size_t len = file.readBytesUntil('\n', buf, sizeof(buf));
  file.close();
  if (len < 19) return false;

  int y, mo, d, h, mi, s;
  if (sscanf(buf, "%04d-%02d-%02d %02d:%02d:%02d", &y, &mo, &d, &h, &mi, &s) != 6) return false;

  start_time = DateTime(y, mo, d, h, mi, s);
  Serial.print("載入上次時間: "); Serial.println(buf);
  return true;
}

static void writeLastTimeUnlocked(DateTime time) {
  SD.remove(LAST_TIME_FILE);
  File time_file = SD.open(LAST_TIME_FILE, FILE_WRITE);
  if (time_file) {
    char buf[24];
    sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d", time.year(), time.month(), time.day(), time.hour(), time.minute(), time.second());
    time_file.println(buf);
    time_file.close();
  } else {
    Serial.println(F("[time] 無法寫入 lasttime.txt"));
  }
}

void updateLastTimeToSD(DateTime time) {
  if (!takeSd(2000)) {
    Serial.println(F("[time] SD busy"));
    return;
  }
  writeLastTimeUnlocked(time);
  giveSd();
}

DateTime getCurrentTime() {
  unsigned long elapsed = millis() - start_millis;
  // 處理溢位
  if (millis() < start_millis) {
    elapsed = (0xFFFFFFFF - start_millis) + millis();
  }
  return start_time + TimeSpan(elapsed / 1000);
}

void buttonISR() { button_pressed = true; }

void loop() {
  // 不再使用
/*
  unsigned long now_millis = millis();
  DateTime now = getCurrentTime();
  static unsigned long last_record = 0;
  const long RECORD_INTERVAL = 60000;

  if (button_pressed) {
    Serial.println("button press");
    button_pressed = false;
    delay(200);
    if (digitalRead(BUTTON_PIN) == LOW) toggleScreen();
  }

  if (now_millis - last_record >= RECORD_INTERVAL) {
    last_record = now_millis;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h) && t > -40 && t < 80 && h >= 0 && h <= 100) {
      // 合理範圍內的數據
      logToSD(t, h, now);
      updateLastTimeToSD(now);  // 更新時間到 SD
      drawGraphFromSD();
    } else {
      Serial.println("Error: Invalid sensor data.");
    }
  }

  static unsigned long last_display = 0;
  if (millis() - last_display > 1000) {
    last_display = millis();
    updateTopLine(now);
  }

  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "CLEAR") {
      clearCSV();
      Serial.println("📁 temp.csv 已清空");
    }
  }
*/
}

void checkStack(const char* taskName) {
  UBaseType_t stackLeft = uxTaskGetStackHighWaterMark(NULL);
  if (stackLeft < 50) {
    Serial.print("["); Serial.print(taskName); Serial.print("] ⚠️ Stack low: ");
    Serial.println(stackLeft);
  }
}

void TaskRecordSensor(void *pvParameters) {
  const TickType_t interval = 2000 / portTICK_PERIOD_MS;  // 每 2 秒執行一次
  TickType_t lastWakeTime = xTaskGetTickCount();

  static unsigned long lastLogMillis = 0;

  for (;;) {
    DateTime now = getCurrentTime();
    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isnan(t) && !isnan(h) && t > -40 && t < 80 && h >= 0 && h <= 100) {
      if (!isAdjustingTime) {
        updateTopLine(t, h, now);  // ✅ 每 2 秒更新畫面
      }


      // 寫入失敗就留到下一輪再試，不要把這一次計時吃掉
      if (millis() - lastLogMillis >= 60000UL) {
        if (logToSD(t, h, now)) lastLogMillis = millis();
      }
    } else {
      Serial.println("[RecordSensor] 感測值異常");
    }

    checkStack("RecordSensor");
    vTaskDelayUntil(&lastWakeTime, interval);
  }
}

void TaskUpdateDisplay(void *pvParameters) {
  unsigned long lastTrimMs = millis();

  for (;;) {
    if (graphDirty) {
      graphDirty = false;
      if (!drawGraphFromSD()) graphDirty = true;
    }

    if (millis() - lastTrimMs >= 60000UL) {
      lastTrimMs = millis();
      trimOldRecords();
    }

    checkStack("UpdateDisplay");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void TaskSerialCommand(void *pvParameters) {

  for (;;) {
    SerialCommand();
    checkStack("SerialCmd");

    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

void SerialCommand(void) {
  static char cmd[80];
  static uint8_t len = 0;

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (len + 1 < sizeof(cmd)) cmd[len++] = c;
      continue;
    }

    cmd[len] = '\0';
    len = 0;

    char* start = cmd;
    while (*start == ' ' || *start == '\t') start++;
    int end = strlen(start);
    while (end > 0 && (start[end - 1] == ' ' || start[end - 1] == '\t')) {
      start[--end] = '\0';
    }

    if (strcmp(start, "CLEAR") == 0) {
      if (clearCSV()) Serial.println(F("temp.csv 已清空"));
      else Serial.println(F("fail to erase"));
    } else if (strcmp(start, "GETTIME") == 0) {
      DateTime now = getCurrentTime();
      char buf[25];
      sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d",
              now.year(), now.month(), now.day(),
              now.hour(), now.minute(), now.second());
      Serial.print(F("TIME "));
      Serial.println(buf);
    } else if (strncmp(start, "SETTIME", 7) == 0) {
      delay(500);
      int y, mo, d, h, mi, s;
      if (sscanf(start, "SETTIME %04d-%02d-%02d %02d:%02d:%02d",
                 &y, &mo, &d, &h, &mi, &s) == 6) {
        start_time = DateTime(y, mo, d, h, mi, s);
        start_millis = millis();
        updateLastTimeToSD(start_time);
        Serial.println(F("時間已更新！"));
      } else {
        Serial.println(F("SETTIME 格式錯誤，應為 yyyy-MM-dd HH:mm:ss"));
      }
    }
  }
}


void TaskButtonHandler(void *pvParameters) {
  static unsigned long lastPressMillis = 0;
  static bool lastState = HIGH;
  const unsigned long LONG_PRESS_DURATION = 1000;
  const unsigned long TIMEOUT_DURATION = 10000;
  
  for (;;) {
    bool currentState = digitalRead(BUTTON_PIN);
    unsigned long now = millis();
    
    if (currentState == LOW && lastState == HIGH) {
      lastPressMillis = now;
    }
    
    if (currentState == HIGH && lastState == LOW) {
      unsigned long pressDuration = now - lastPressMillis;
      isAdjustingTime = true;

      if (pressDuration >= LONG_PRESS_DURATION) {
        // 長按：切換模式
        if (adjustMode == NONE) {
          adjustMode = ADJUST_MINUTE;
          adjustTime = getCurrentTime();
          adjustStartMillis = now;
          Serial.println("進入校正模式：分鐘");
        } else if (adjustMode == ADJUST_MINUTE) {
          adjustMode = ADJUST_HOUR;
          Serial.println("切換到校正模式：小時");
        } else {
          adjustMode = ADJUST_MINUTE;
          Serial.println("切換到校正模式：分鐘");
        }
      } else {
        // 短按：+1
        if (adjustMode == ADJUST_MINUTE) {
          adjustTime = adjustTime + TimeSpan(0, 0, 1, 0);
          Serial.print("分鐘 +1 → "); Serial.println(adjustTime.minute());
        } else if (adjustMode == ADJUST_HOUR) {
          adjustTime = adjustTime + TimeSpan(0, 1, 0, 0);
          Serial.print("小時 +1 → "); Serial.println(adjustTime.hour());
        }
      }
      drawTimeAdjustHint(adjustMode, adjustTime);
    }
    
    // timeout
    if (adjustMode != NONE && (now - adjustStartMillis > TIMEOUT_DURATION)) {
      start_time = DateTime(adjustTime.year(), adjustTime.month(), adjustTime.day(),
                            adjustTime.hour(), adjustTime.minute(), 0);
      start_millis = millis();
      updateLastTimeToSD(start_time);
      Serial.println("⏱ 校時完成並儲存！");
      adjustMode = NONE;
      isAdjustingTime = false;
    }
    
    lastState = currentState;

    SerialCommand();

    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void clearTopLineArea() {
  int screen_w = mylcd.Get_Display_Width();
  int section_w = screen_w / 5;
  int center_datetime = section_w * 3 / 2;
  int center_temp = section_w * 3 + section_w / 2;
  int center_hum = section_w * 4 + section_w / 2;

  uint8_t text_size = 4;
  int char_w = 6 * text_size;

  // 清空三個區塊（用空白字串覆蓋）
  printWithBackground("        ", center_datetime - 8 * char_w / 2, 40, BLACK, BLACK, text_size);
  printWithBackground("        ", center_temp - 8 * char_w / 2, 40, BLACK, BLACK, text_size);
  printWithBackground("        ", center_hum - 8 * char_w / 2, 40, BLACK, BLACK, text_size);
}


void drawTimeAdjustHint(TimeAdjustMode mode, DateTime time) {
  int screen_w = mylcd.Get_Display_Width();
  int section_w = screen_w / 5;

  int center_datetime = section_w * 3 / 2;       // 左 3/5 的中間
  int center_temp = section_w * 3 + section_w / 2; // 第 4 等分
  int center_hum = section_w * 4 + section_w / 2;  // 第 5 等分

  clearTopLineArea();

  uint8_t text_size = 4;
  int char_w = 6 * text_size;

  // 顯示時間 + 日期
  char datetime_str[20];
  sprintf(datetime_str, "%02d:%02d %02d/%02d", time.hour(), time.minute(), time.month(), time.day());
  printWithBackground(datetime_str, center_datetime - strlen(datetime_str) * char_w / 2, 40, WHITE, BLACK, text_size);

  // 顯示模式提示
  const char* mode_str = "";
  uint16_t mode_color = WHITE;
  if (mode == ADJUST_MINUTE) {
    mode_str = "ADJUST_MINUTE";
    mode_color = YELLOW;
  } else if (mode == ADJUST_HOUR) {
    mode_str = "ADJUST_HOUR";
    mode_color = RED;
  }

  printWithBackground(mode_str, center_temp - strlen(mode_str) * char_w / 2, 40, mode_color, BLACK, text_size);
}

void toggleScreen() {
  if (screen_on) {
    if (takeLcd(2000)) {
      mylcd.Write_Cmd(0x28);
      giveLcd();
    }
    Serial.println("off");
  } else {
    if (takeLcd(2000)) {
      mylcd.Write_Cmd(0x29);
      giveLcd();
    }
    Serial.println("on");
    drawUI();
    graphDirty = false;
    if (!drawGraphFromSD()) graphDirty = true;
  }
  screen_on = !screen_on;
}


bool clearCSV() {
  if (!takeSd(2000)) {
    Serial.println(F("[clear] SD busy"));
    return false;
  }
  SD.remove(FILENAME);
  SD.remove(TEMP_NAME);
  SD.remove(TRIM_READY);
  File file = SD.open(FILENAME, FILE_WRITE);
  bool ok = false;
  if (file) {
    ok = file.println(CSV_HEADER) > 0;
    file.close();
  }
  giveSd();
  graphDirty = true;
  return ok;
}

void drawUI() {
  if (!takeLcd(2000)) return;
  mylcd.Fill_Screen(BLACK);
  mylcd.Set_Text_Size(2);
  mylcd.Set_Text_colour(WHITE);
  mylcd.Set_Text_Back_colour(BLACK);
  mylcd.Print_String("12-Hour Temp/Hum Monitor", 10, 10);
  drawAxes();
  drawYAxisLabels();
  giveLcd();
}

void drawAxes() {
  mylcd.Set_Draw_color(WHITE);
  mylcd.Draw_Rectangle(GRAPH_X - 1, GRAPH_Y - 1, GRAPH_X + GRAPH_W + 1, GRAPH_BOTTOM + 1);

  mylcd.Set_Text_Size(2);
  for (int t = TEMP_MIN; t <= TEMP_MAX; t += 2) {
    int y = tempToY(t);
    mylcd.Set_Text_colour(YELLOW);
    mylcd.Set_Text_Back_colour(BLACK);
    mylcd.Draw_Fast_HLine(GRAPH_X - 5, y, 5);
  }
  for (int h = HUM_MIN; h <= HUM_MAX; h += 10) {
    int y = humToY(h);
    mylcd.Set_Text_colour(CYAN);
    mylcd.Set_Text_Back_colour(BLACK);
    mylcd.Draw_Fast_HLine(GRAPH_X - 5, y, 5);
  }
}

void updateTopLine(float t, float h, DateTime now) {
  char datetime_str[20];
  sprintf(datetime_str, "%02d:%02d %02d/%02d", now.hour(), now.minute(), now.month(), now.day());

  char temp_str[12];
  sprintf(temp_str, "%dC", (int)t);

  char hum_str[8];
  sprintf(hum_str, "%d%%", (int)h);

  static char last_datetime[20] = "";
  static char last_temp[12] = "";
  static char last_hum[8] = "";

  bool changed = strcmp(datetime_str, last_datetime) != 0 ||
                 strcmp(temp_str, last_temp) != 0 ||
                 strcmp(hum_str, last_hum) != 0;

  if (!changed) return;

  int screen_w = mylcd.Get_Display_Width();
  int section_w = screen_w / 5;

  int center_datetime = section_w * 3 / 2;       // 左 3/5 的中間
  int center_temp = section_w * 3 + section_w / 2; // 第 4 等分
  int center_hum = section_w * 4 + section_w / 2;  // 第 5 等分

  clearTopLineArea();

  uint8_t text_size = 4;
  int char_w = 6 * text_size;

  // 顯示時間 + 日期
  printWithBackground(datetime_str, center_datetime - strlen(datetime_str) * char_w / 2, 40, WHITE, BLACK, text_size);

  // 顯示溫度與濕度
  printWithBackground(temp_str, center_temp - strlen(temp_str) * char_w / 2, 40, YELLOW, BLACK, text_size);
  printWithBackground(hum_str, center_hum - strlen(hum_str) * char_w / 2, 40, CYAN, BLACK, text_size);

  strcpy(last_datetime, datetime_str);
  strcpy(last_temp, temp_str);
  strcpy(last_hum, hum_str);

  // 🔧 新增序列輸出
  // [TopLine] 13:28 12/24 | 26C | 60%
  Serial.print("[TopLine] ");
  Serial.print(datetime_str);
  Serial.print(" | ");
  Serial.print(temp_str);
  Serial.print(" | ");
  Serial.println(hum_str);

}


bool logToSD(float t, float h, DateTime time) {
  if (!takeSd(800)) {
    Serial.println(F("[log] SD busy"));
    return false;
  }

  bool ok = false;
  File file = SD.open(FILENAME, FILE_WRITE);
  if (!file) {
    Serial.println(F("[log] 無法開啟 temp.csv"));
  } else {
    if (file.size() == 0) file.println(CSV_HEADER);
    file.seek(file.size());

    char timestamp[20];
    sprintf(timestamp, "%04d-%02d-%02d %02d:%02d:00",
            time.year(), time.month(), time.day(),
            time.hour(), time.minute());

    size_t wrote = file.print(timestamp);
    wrote += file.print(',');
    wrote += file.print(t, 1);
    wrote += file.print(',');
    wrote += file.println((int)h);
    file.close();
    if (wrote == 0) {
      Serial.println(F("[log] 寫入失敗"));
    } else {
      writeLastTimeUnlocked(time);
      ok = true;
    }
  }

  giveSd();
  if (ok) graphDirty = true;
  return ok;
}

void trimOldRecords() {
  if (!takeSd(2500)) {
    Serial.println(F("[trim] SD busy"));
    return;
  }

  File file = SD.open(FILENAME, FILE_READ);
  if (!file) {
    giveSd();
    return;
  }
  int totalLines = countAllLines(file);
  file.close();

  int dataLines = totalLines > 0 ? totalLines - 1 : 0;
  if (dataLines <= MAX_RECORDS + TRIM_MARGIN) {
    giveSd();
    return;
  }

  int toSkip = (dataLines - MAX_RECORDS) + 1;
  Serial.print(F("[trim] 資料 "));
  Serial.print(dataLines);
  Serial.print(F(" 筆，保留最新 "));
  Serial.println(MAX_RECORDS);

  SD.remove(TEMP_NAME);
  SD.remove(TRIM_READY);

  file = SD.open(FILENAME, FILE_READ);
  File dst = SD.open(TEMP_NAME, FILE_WRITE);
  if (!file || !dst) {
    if (file) file.close();
    if (dst) dst.close();
    SD.remove(TEMP_NAME);
    Serial.println(F("[trim] 無法建立 temp.tmp"));
    giveSd();
    return;
  }

  dst.println(CSV_HEADER);
  lineReaderStart(&file);

  char line[64];
  int skipped = 0;
  bool writeOk = true;
  while (skipped < toSkip) {
    if (!readCsvLine(line, sizeof(line))) {
      writeOk = false;
      break;
    }
    skipped++;
  }
  while (writeOk && readCsvLine(line, sizeof(line))) {
    if (line[0] == '\0') continue;
    if (dst.println(line) == 0) writeOk = false;
  }

  file.close();
  dst.close();
  lineFile = NULL;

  if (!writeOk) {
    SD.remove(TEMP_NAME);
    Serial.println(F("[trim] 寫入 temp.tmp 失敗，原檔保留"));
    giveSd();
    return;
  }

  File marker = SD.open(TRIM_READY, FILE_WRITE);
  if (!marker) {
    SD.remove(TEMP_NAME);
    Serial.println(F("[trim] 無法寫入 trim.rdy，取消覆蓋"));
    giveSd();
    return;
  }
  marker.close();

  SD.remove(FILENAME);
  File src = SD.open(TEMP_NAME, FILE_READ);
  File out = SD.open(FILENAME, FILE_WRITE);
  bool copied = src && out && copyFile(src, out);
  if (src) src.close();
  if (out) out.close();
  if (!copied) {
    SD.remove(FILENAME);
    Serial.println(F("[trim] 覆蓋失敗，開機時會用 temp.tmp 還原"));
    giveSd();
    return;
  }

  SD.remove(TEMP_NAME);
  SD.remove(TRIM_READY);
  Serial.println(F("[trim] 裁切完成"));
  giveSd();
}

int tempToY(float temp) {
  temp = constrain(temp, TEMP_MIN, TEMP_MAX);
  int y = GRAPH_BOTTOM - (int)((temp - TEMP_MIN) * GRAPH_H / (TEMP_MAX - TEMP_MIN));
  return constrain(y, GRAPH_Y, GRAPH_BOTTOM);
}

int humToY(float hum) {
  hum = constrain(hum, HUM_MIN, HUM_MAX);
  int y = GRAPH_BOTTOM - (int)((hum - HUM_MIN) * GRAPH_H / (HUM_MAX - HUM_MIN));
  return constrain(y, GRAPH_Y, GRAPH_BOTTOM);
}

void drawYAxisLabels() {
  mylcd.Set_Text_Size(2);

  for (int t = TEMP_MIN; t <= TEMP_MAX; t += 2) {
    int y = tempToY(t);
    mylcd.Set_Text_colour(YELLOW);
    char buf[8];
    sprintf(buf, "%dC", t);
    mylcd.Print_String(buf, 0, y - 6);
  }

  for (int h = HUM_MIN; h <= HUM_MAX; h += 10) {
    int y = humToY(h);
    mylcd.Set_Text_colour(CYAN);
    char buf[8];
    sprintf(buf, "%d%%", h);
    mylcd.Print_String(buf, 40, y - 6);
  }
}

static bool parseHistoryLine(const char* line, uint8_t* hh, uint8_t* mm, int16_t* temp10, uint8_t* hum) {
  int y, mo, d, h, mi;
  const char* comma = strchr(line, ',');
  if (!comma) return false;
  if (sscanf(line, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return false;
  const char* comma2 = strchr(comma + 1, ',');
  if (!comma2) return false;

  float temp = atof(comma + 1);
  float humidity = atof(comma2 + 1);
  if (h < 0) h = 0;
  if (h > 23) h = 23;
  if (mi < 0) mi = 0;
  if (mi > 59) mi = 59;
  *hh = (uint8_t)h;
  *mm = (uint8_t)mi;

  float scaled = temp * 10.0f;
  *temp10 = (int16_t)(scaled >= 0 ? scaled + 0.5f : scaled - 0.5f);
  int hv = (int)(humidity >= 0 ? humidity + 0.5f : humidity - 0.5f);
  if (hv < 0) hv = 0;
  if (hv > 100) hv = 100;
  *hum = (uint8_t)hv;
  return true;
}

static int loadGraphPoints(GraphTick* ticks, int tickCapacity, int* tickCount) {
  *tickCount = 0;
  File file = SD.open(FILENAME, FILE_READ);
  if (!file) return -1;

  int totalLines = countAllLines(file);
  file.close();
  int dataLines = totalLines > 0 ? totalLines - 1 : 0;
  if (dataLines <= 0) return 0;

  int keep = dataLines;
  if (GRAPH_W > 0 && keep > GRAPH_W) keep = GRAPH_W;
  if (keep > GRAPH_POINT_MAX) keep = GRAPH_POINT_MAX;
  int skipData = dataLines - keep;

  file = SD.open(FILENAME, FILE_READ);
  if (!file) return -1;
  lineReaderStart(&file);

  char line[64];
  int skipped = 0;
  int toSkip = skipData + 1;
  while (skipped < toSkip) {
    if (!readCsvLine(line, sizeof(line))) {
      file.close();
      lineFile = NULL;
      return -1;
    }
    skipped++;
  }

  int tickInterval = keep / 4;
  if (tickInterval < 1) tickInterval = 1;
  int count = 0;
  while (count < keep && readCsvLine(line, sizeof(line))) {
    if (line[0] == '\0') continue;
    uint8_t hh, mm, hum;
    int16_t temp10;
    if (!parseHistoryLine(line, &hh, &mm, &temp10, &hum)) continue;
    graphTemp10[count] = temp10;
    graphHum[count] = hum;
    if (ticks != NULL && *tickCount < tickCapacity && (count % tickInterval) == 0) {
      ticks[*tickCount].x = GRAPH_X + count;
      ticks[*tickCount].hh = hh;
      ticks[*tickCount].mm = mm;
      (*tickCount)++;
    }
    count++;
  }
  file.close();
  lineFile = NULL;
  if (count <= 0) return -1;
  return count;
}

static void printText(const char* s, int x, int y, uint16_t textColor, uint16_t bgColor, uint8_t text_size) {
  int char_w = 6 * text_size;
  int char_h = 8 * text_size;
  int text_w = strlen(s) * char_w;
  mylcd.Set_Text_Size(text_size);
  mylcd.Set_Draw_color(bgColor);
  mylcd.Fill_Rectangle(x, y, x + text_w, y + char_h);
  mylcd.Set_Text_colour(textColor);
  mylcd.Set_Text_Back_colour(bgColor);
  mylcd.Print_String(s, x, y);
}

void printWithBackground(const char* s, int x, int y, uint16_t textColor, uint16_t bgColor, uint8_t text_size) {
  if (!takeLcd(8000)) return;
  printText(s, x, y, textColor, bgColor, text_size);
  giveLcd();
}

static bool drawGraphPoints(int count, const GraphTick* ticks, int tickCount) {
  if (!takeLcd(8000)) {
    Serial.println(F("[draw] LCD busy"));
    return false;
  }

  mylcd.Set_Draw_color(BLACK);
  mylcd.Fill_Rectangle(GRAPH_X, GRAPH_Y, GRAPH_X + GRAPH_W, GRAPH_BOTTOM);

  int last_x = -1, last_temp_y = -1, last_hum_y = -1;
  int n = count;
  if (n > GRAPH_POINT_MAX) n = GRAPH_POINT_MAX;
  for (int i = 0; i < n; i++) {
    int x = GRAPH_X + i;
    int temp_y = tempToY(graphTemp10[i] / 10.0f);
    int hum_y = humToY((float)graphHum[i]);
    if (last_x >= 0) {
      mylcd.Set_Draw_color(YELLOW);
      mylcd.Draw_Line(last_x, last_temp_y, x, temp_y);
      mylcd.Set_Draw_color(CYAN);
      mylcd.Draw_Line(last_x, last_hum_y, x, hum_y);
    }
    last_x = x;
    last_temp_y = temp_y;
    last_hum_y = hum_y;
  }

  if (last_x >= 0) {
    mylcd.Set_Draw_color(YELLOW);
    mylcd.Fill_Circle(last_x, last_temp_y, 2);
    mylcd.Set_Draw_color(CYAN);
    mylcd.Fill_Circle(last_x, last_hum_y, 2);
  }

  mylcd.Set_Draw_color(BLACK);
  mylcd.Fill_Rectangle(GRAPH_X, GRAPH_BOTTOM + 6, GRAPH_X + GRAPH_W, GRAPH_BOTTOM + 25);
  for (int i = 0; i < tickCount; i++) {
    mylcd.Draw_Fast_VLine(ticks[i].x, GRAPH_BOTTOM, 5);
    char buf[6];
    sprintf(buf, "%02d:%02d", ticks[i].hh, ticks[i].mm);
    int text_w = strlen(buf) * 12;
    int tx = ticks[i].x - text_w / 2;
    tx = constrain(tx, 0, mylcd.Get_Display_Width() - text_w);
    printText(buf, tx, GRAPH_BOTTOM + 10, WHITE, BLACK, 2);
  }

  giveLcd();
  return true;
}

bool drawGraphFromSD() {
  if (!takeSd(2500)) {
    Serial.println(F("[draw] SD busy"));
    return false;
  }

  GraphTick ticks[5];
  int tickCount = 0;
  int count = loadGraphPoints(ticks, 5, &tickCount);
  giveSd();
  if (count < 0) return false;
  return drawGraphPoints(count, ticks, tickCount);
}
