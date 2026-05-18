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

struct Record {
  DateTime time;
  float temp;
  float hum;
};

SemaphoreHandle_t sdMutex;
SemaphoreHandle_t lcdMutex;


#define DISPLAY_TASK_SIZE 1024
#define TRIM_BUFFER_SIZE 1792  // 可改成 2048、8192 等視 SRAM 而定
char trimBuffer[TRIM_BUFFER_SIZE];  // ✅ 放在全域，減少堆疊壓力
//#define DEBUG_TRIM_LOG  // 註解掉這行即可關閉 trimOldRecords 的 log

enum TimeAdjustMode { NONE, ADJUST_MINUTE, ADJUST_HOUR };
TimeAdjustMode adjustMode = NONE;

unsigned long adjustStartMillis = 0;
DateTime adjustTime;  // 暫存調整中的時間
volatile bool isAdjustingTime = false;

bool force_set_compile_time = false;


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

int countDataLines() {
  File file = SD.open(FILENAME);
  if (!file) return 0;

  // 跳過 header
  file.readStringUntil('\n');

  int lines = 0;
  while (file.available()) {
    if (file.readStringUntil('\n').length() > 0) lines++;
  }
  file.close();
  return lines;
}

void ensureFullData() {
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    Serial.println("[ERROR] 無法取得 SD mutex，跳過補資料");
    return;
  }

  int currentLines = countDataLines();  // 計算資料筆數（不含 header）

  if (currentLines >= MAX_RECORDS) {
    Serial.print("資料已足夠:");
    Serial.print(currentLines);
    Serial.println(" 筆，無需補充");
    xSemaphoreGive(sdMutex);
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
    xSemaphoreGive(sdMutex);
    return;
  }

  // 確保有 header
  if (file.size() == 0) {
    file.println("Timestamp,Temperature_C,Humidity_%");
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
  xSemaphoreGive(sdMutex);

  Serial.print("補充完成！總筆數：");
  Serial.println(countDataLines());
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

void updateLastTimeToSD(DateTime time) {
  SD.remove(LAST_TIME_FILE);
  File time_file = SD.open(LAST_TIME_FILE, FILE_WRITE);
  if (time_file) {
    char buf[20];
    sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d", time.year(), time.month(), time.day(), time.hour(), time.minute(), time.second());
    time_file.println(buf);
    time_file.close();
    // Serial.print("[updateLastTimeToSD] save the time to sd card"); Serial.println(buf);
  }else{
    Serial.println("[updateLastTimeToSD] fail to open lasttime.txt");
  }
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


      // ✅ 每 60 秒記錄一次資料
      if (millis() - lastLogMillis >= 60000) {
        if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
          logToSD(t, h, now);
          updateLastTimeToSD(now);
          xSemaphoreGive(sdMutex);
        } else {
          Serial.println("[RecordSensor] SD busy, skip log");
        }

        lastLogMillis = millis();
      }
    } else {
      Serial.println("[RecordSensor] 感測值異常");
    }

    checkStack("RecordSensor");
    vTaskDelayUntil(&lastWakeTime, interval);
  }
}

void TaskUpdateDisplay(void *pvParameters) {
  const TickType_t interval = 60000 / portTICK_PERIOD_MS;
  TickType_t lastWakeTime = xTaskGetTickCount();
  
  for (;;) {
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
      drawGraphFromSD();  // ✅ 圖表更新可能較久，獨立執行
      
      trimOldRecords();

      xSemaphoreGive(sdMutex);
    }

    checkStack("UpdateDisplay");
    vTaskDelayUntil(&lastWakeTime, interval);
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
  String cmdBuffer = "";

  while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n') {
        cmdBuffer.trim();
        if (cmdBuffer == "CLEAR") {
          if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
            clearCSV();
            xSemaphoreGive(sdMutex);
            Serial.println("📁 temp.csv 已清空");
          }else{
            Serial.println("fail to erase");
          }
        } else if (cmdBuffer == "GETTIME") {
          DateTime now = getCurrentTime();
          char buf[25];
          sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d",
                  now.year(), now.month(), now.day(),
                  now.hour(), now.minute(), now.second());
          Serial.print("TIME "); Serial.println(buf);
        } else if (cmdBuffer.startsWith("SETTIME")) {
          delay(500);
          int y, mo, d, h, mi, s;
          if (sscanf(cmdBuffer.c_str(), "SETTIME %04d-%02d-%02d %02d:%02d:%02d",
                     &y, &mo, &d, &h, &mi, &s) == 6) {
            start_time = DateTime(y, mo, d, h, mi, s);
            start_millis = millis();
            if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
              updateLastTimeToSD(start_time);
              xSemaphoreGive(sdMutex);
            } else {
              Serial.println("SD busy, skip update");
            }

            Serial.println("時間已更新！");
          } else {
            Serial.println("SETTIME 格式錯誤，應為 yyyy-MM-dd HH:mm:ss");
          }
        }

        cmdBuffer = "";
      } else {
        cmdBuffer += c;
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
    mylcd.Write_Cmd(0x28);
    Serial.println("off");
  } else {
    mylcd.Write_Cmd(0x29);
    Serial.println("on");
    drawUI();
    drawGraphFromSD();
  }
  screen_on = !screen_on;
}


void clearCSV() {
  SD.remove(FILENAME);
  File file = SD.open(FILENAME, FILE_WRITE);
  if (file) {
    file.println("Time,Temperature,Humidity");
    file.close();
  }
}

void drawUI() {
  mylcd.Fill_Screen(BLACK);
  mylcd.Set_Text_Size(2);
  mylcd.Set_Text_colour(WHITE);
  mylcd.Set_Text_Back_colour(BLACK);
  mylcd.Print_String("12-Hour Temp/Hum Monitor", 10, 10);
  drawAxes();
  drawYAxisLabels();
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


void logToSD(float t, float h, DateTime time) {
  File file = SD.open(FILENAME, FILE_WRITE);
  if (!file) return;

  if (file.size() == 0) {
    file.println("Timestamp,Temperature_C,Humidity_%");
  }
  
  file.seek(file.size());  // 移到檔尾

  char timestamp[20];
  sprintf(timestamp, "%04d-%02d-%02d %02d:%02d:00",
          time.year(), time.month(), time.day(),
          time.hour(), time.minute());

  file.print(timestamp);
  file.print(",");
  file.print(t, 1);
  file.print(",");
  file.println((int)h);
  file.close();
}

int countLines(const char* filename) {
  File file = SD.open(filename);
  if (!file) return 0;
  int lines = 0;
  while (file.available()) {
    if (file.read() == '\n') lines++;
  }
  int extra = (file.position() > 0 && file.peek() != -1) ? 1 : 0;
  file.close();
  return lines + extra;
}

void trimOldRecords() {
  // ✅ 條件觸發 trim：資料超過 MAX_RECORDS 且距離上次 trim 足夠久

  int lines = countLines(FILENAME);
  if (lines < MAX_RECORDS + 50) {
    return;
  }
  Serial.print("[trimOldRecords] ");
  Serial.print(lines);
  Serial.println(" lines");

  Serial.print("[trimOldRecords] ");
  Serial.print(millis());
  Serial.println(" 開始 trimOldRecords...");
  #ifdef DEBUG_TRIM_LOG
  Serial.print("[trimOldRecords] Stack left: ");
  Serial.println(uxTaskGetStackHighWaterMark(NULL));
  #endif

  File src = SD.open(FILENAME, FILE_READ);
  if (!src) {
    #ifdef DEBUG_TRIM_LOG
    Serial.println("[trimOldRecords] 開啟原始檔失敗");
    #endif
    return;
  }

  int totalLines = 0;
  while (src.available()) {
    if (src.read() == '\n') totalLines++;
  }
  src.close();

  #ifdef DEBUG_TRIM_LOG
  Serial.print("[trimOldRecords] 總行數（含 header）: ");
  Serial.println(totalLines);
  #endif

  if (totalLines <= MAX_RECORDS + 1) {
    #ifdef DEBUG_TRIM_LOG
    Serial.println("[trimOldRecords] 資料尚未超過 MAX_RECORDS，無需裁剪");
    #endif
    return;
  }

  int skipLines = totalLines - MAX_RECORDS;
  #ifdef DEBUG_TRIM_LOG
  Serial.print("[trimOldRecords] 將跳過前 ");
  Serial.print(skipLines);
  Serial.println(" 行");
  #endif

  src = SD.open(FILENAME, FILE_READ);
  if (!src) {
    #ifdef DEBUG_TRIM_LOG
    Serial.println("[trimOldRecords] 第二次開啟原始檔失敗");
    #endif
    return;
  }

  File dst = SD.open("temp.tmp", FILE_WRITE);
  if (!dst) {
    #ifdef DEBUG_TRIM_LOG
    Serial.println("[trimOldRecords] 無法建立 temp.tmp");
    #endif
    src.close();
    return;
  }

  // 跳過前 skipLines 行
  int skipped = 0;
  while (src.available() && skipped < skipLines) {
    if (src.read() == '\n') skipped++;
  }

  // 寫入 header
  dst.println("Timestamp,Temperature_C,Humidity_%");

  while (src.available()) {
    size_t n = src.readBytes(trimBuffer, TRIM_BUFFER_SIZE);
    dst.write((uint8_t*)trimBuffer, n);
  }

  src.close(); dst.close();

  #ifdef DEBUG_TRIM_LOG
  Serial.println("[trimOldRecords] 資料搬移完成，準備覆蓋原始檔");
  #endif

  SD.remove(FILENAME);
  File final = SD.open(FILENAME, FILE_WRITE);
  File temp = SD.open("temp.tmp", FILE_READ);
  if (final && temp) {
    while (temp.available()) {
      size_t n = temp.readBytes(trimBuffer, TRIM_BUFFER_SIZE);
      final.write((uint8_t*)trimBuffer, n);
    }

    final.close(); temp.close();
    SD.remove("temp.tmp");
    #ifdef DEBUG_TRIM_LOG
    Serial.println("[trimOldRecords] 成功覆蓋原始檔並刪除 temp.tmp");
    #endif
  } else {
    #ifdef DEBUG_TRIM_LOG
    Serial.println("[trimOldRecords] 覆蓋失敗，請檢查 SD 狀態");
    #endif
  }

  Serial.print("[trimOldRecords] ");
  Serial.print(millis());
  Serial.println(" 完成搬移");
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

void drawGraphFromSD() {
  const int MAX_POINTS = GRAPH_W;
  const int TICK_COUNT = 4;
  const int BYTES_PER_LINE = 40;
  const int LINES_PER_BATCH = TRIM_BUFFER_SIZE / BYTES_PER_LINE;

  File file = SD.open(FILENAME);
  if (!file) {
    Serial.println("無法開啟 temp.csv");
    return;
  }

  file.readStringUntil('\n'); // 跳過 header

  // 預先計算總筆數
  int total_lines = 0;
  while (file.available()) {
    if (file.readStringUntil('\n').length() > 0) total_lines++;
  }
  file.close();

  int skip_lines = max(0, total_lines - MAX_POINTS);

  // 重新開啟並跳過 header + skip_lines
  file = SD.open(FILENAME);
  file.readStringUntil('\n'); // 跳過 header
  for (int i = 0; i < skip_lines; i++) {
    file.readStringUntil('\n');
  }


  mylcd.Set_Draw_color(BLACK);
  mylcd.Fill_Rectangle(GRAPH_X, GRAPH_Y, GRAPH_X + GRAPH_W, GRAPH_BOTTOM);

  int last_x = -1, last_temp_y = -1, last_hum_y = -1;
  int index = 0;
  int tick_interval = MAX_POINTS / TICK_COUNT;
  struct Tick { int x; DateTime time; } ticks[TICK_COUNT + 1];
  int tick_index = 0;

  while (file.available() && index < MAX_POINTS) {
    for (int i = 0; i < LINES_PER_BATCH && file.available() && index < MAX_POINTS; i++) {
      size_t len = file.readBytesUntil('\n', trimBuffer, TRIM_BUFFER_SIZE - 1);
      if (len == 0) continue;
      trimBuffer[len] = '\0';

      char* token = strtok(trimBuffer, ",");
      if (!token) continue;

      int y, mo, d, hr, mi;
      if (sscanf(token, "%d-%d-%d %d:%d", &y, &mo, &d, &hr, &mi) != 5) continue;
      DateTime record_time(y, mo, d, hr, mi, 0);

      token = strtok(NULL, ",");
      if (!token) continue;
      float t = atof(token);

      token = strtok(NULL, ",");
      if (!token) continue;
      float h = atof(token);

      int x = GRAPH_X + index;
      int temp_y = tempToY(t);
      int hum_y = humToY(h);

      if (last_x >= 0) {
        mylcd.Set_Draw_color(YELLOW); mylcd.Draw_Line(last_x, last_temp_y, x, temp_y);
        mylcd.Set_Draw_color(CYAN);   mylcd.Draw_Line(last_x, last_hum_y, x, hum_y);
      }

      last_x = x; last_temp_y = temp_y; last_hum_y = hum_y;

      if (tick_index <= TICK_COUNT && index % tick_interval == 0) {
        ticks[tick_index++] = {x, record_time};
      }

      index++;
    }
  }
  file.close();

  // 畫最後一點
  if (last_x >= 0) {
    mylcd.Set_Draw_color(YELLOW); mylcd.Fill_Circle(last_x, last_temp_y, 2);
    mylcd.Set_Draw_color(CYAN);   mylcd.Fill_Circle(last_x, last_hum_y, 2);
  }

  // 畫刻度
  mylcd.Set_Draw_color(BLACK);
  mylcd.Fill_Rectangle(GRAPH_X, GRAPH_BOTTOM + 6, GRAPH_X + GRAPH_W, GRAPH_BOTTOM + 20);

  for (int i = 0; i < tick_index; i++) {
    mylcd.Draw_Fast_VLine(ticks[i].x, GRAPH_BOTTOM, 5);
    char buf[6]; sprintf(buf, "%02d:%02d", ticks[i].time.hour(), ticks[i].time.minute());
    int text_w = strlen(buf) * 6 * 2;
    int x = ticks[i].x - text_w / 2;
    x = constrain(x, 0, mylcd.Get_Display_Width() - text_w);

    printWithBackground(buf, x, GRAPH_BOTTOM + 10, WHITE, BLACK, 2);
  }
}

void printWithBackground(const char* s, int x, int y, uint16_t textColor, uint16_t bgColor, uint8_t text_size) {
  if (xSemaphoreTake(lcdMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    int char_w = 6 * text_size; // 每個字元寬度（根據 Set_Text_Size(2)）
    int char_h = 8 * text_size; // 每個字元高度（根據 Set_Text_Size(2)）
    int text_w = strlen(s) * char_w;
    mylcd.Set_Text_Size(text_size);
    mylcd.Set_Draw_color(bgColor);
    mylcd.Fill_Rectangle(x, y, x + text_w, y + char_h);

    mylcd.Set_Text_colour(textColor);
    mylcd.Set_Text_Back_colour(bgColor); // ✅ 加上這行，確保文字底色一致
    mylcd.Print_String(s, x, y);
    xSemaphoreGive(lcdMutex);
  }
}
