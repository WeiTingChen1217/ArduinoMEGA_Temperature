#include <LCDWIKI_GUI.h>
#include <LCDWIKI_KBV.h>
#include <DHT.h>
#include <SD.h>
#include <SPI.h>
#include <RTClib.h>

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
#define HUM_MIN 50
#define HUM_MAX 100

DateTime start_time;
unsigned long start_millis;
const char LAST_TIME_FILE[] = "lasttime.txt";  // 存啟動時間


#define BUTTON_PIN 2

volatile bool button_pressed = false;
bool screen_on = true;

struct Record {
  DateTime time;
  float temp;
  float hum;
};

void setup() {
  Serial.begin(115200);
  
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);

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
  drawGraphFromSD();
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
  Serial.print(F("編譯時間: "));
  Serial.println(compile_str);

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
    Serial.println(F("採用編譯時間"));
  } else {
    start_time = file_time;
    Serial.println(F("採用檔案時間"));
  }

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
    Serial.print("儲存時間: "); Serial.println(buf);
  }
}

void saveLastTime() {
  SD.remove(LAST_TIME_FILE);
  File file = SD.open(LAST_TIME_FILE, FILE_WRITE);
  if (!file) return;

  char buf[20];
  sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d",
          start_time.year(), start_time.month(), start_time.day(),
          start_time.hour(), start_time.minute(), start_time.second());
  file.println(buf);
  file.close();
  Serial.print("儲存啟動時間: "); Serial.println(buf);
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

void updateTopLine(DateTime now) {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  char time_str[6];
  sprintf(time_str, "%02d:%02d", now.hour(), now.minute());
  char temp_str[12];
  sprintf(temp_str, "%dC", (int)t);
  char hum_str[8];
  sprintf(hum_str, "%d%%", (int)h);

  static char last_time[6] = "";
  static char last_temp[12] = "";
  static char last_hum[8] = "";

  bool changed = strcmp(time_str, last_time) != 0 ||
                 strcmp(temp_str, last_temp) != 0 ||
                 strcmp(hum_str, last_hum) != 0;

  if (!changed) return;

  int screen_w = mylcd.Get_Display_Width();
  int section_w = screen_w / 3;
  int center1 = section_w / 2;
  int center2 = section_w + section_w / 2;
  int center3 = section_w * 2 + section_w / 2;

  mylcd.Set_Draw_color(BLACK);
  mylcd.Fill_Rectangle(0, 30, screen_w, 60);
  mylcd.Set_Text_Size(4);
  mylcd.Set_Text_colour(WHITE);

  auto print_centered = [&](const char* s, int cx, int y) {
    int char_w = 12;
    int w = strlen(s) * char_w;
    int x = cx - w / 2;
    mylcd.Print_String(s, x, y);
  };

  uint8_t text_size = 4;
  int char_w = 6 * text_size;
  
  printWithBackground(time_str, center1 - strlen(time_str) * char_w / 2, 32, WHITE, BLACK, text_size);
  printWithBackground(temp_str, center2 - strlen(temp_str) * char_w / 2, 32, YELLOW, BLACK, text_size);
  printWithBackground(hum_str, center3 - strlen(hum_str) * char_w / 2, 32, CYAN, BLACK, text_size);

  strcpy(last_time, time_str);
  strcpy(last_temp, temp_str);
  strcpy(last_hum, hum_str);
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

  if (countLines(FILENAME) > MAX_RECORDS + 1) {
    trimOldRecords();
  }
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
  File src = SD.open(FILENAME, FILE_READ);
  if (!src) return;

  const int BUFFER_SIZE = 10;
  Record buffer[BUFFER_SIZE];
  int buffer_count = 0;

  src.readStringUntil('\n');               // skip header

  //--- 讀前 BUFFER_SIZE 筆 ---
  while (src.available() && buffer_count < BUFFER_SIZE) {
    String line = src.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    char *tok = strtok((char*)line.c_str(), ",");
    if (!tok) continue;
    int y,mo,d,hr,mi;
    if (sscanf(tok, "%d-%d-%d %d:%d:00", &y,&mo,&d,&hr,&mi) != 5) continue;

    tok = strtok(NULL, ",");
    if (!tok) continue;
    float temp = atof(tok);

    tok = strtok(NULL, ",");
    if (!tok) continue;
    float hum = atof(tok);

    buffer[buffer_count++] = {DateTime(y,mo,d,hr,mi,0), temp, hum};
  }

  //--- 排序緩衝 ---
  for (int i = 0; i < buffer_count-1; i++) {
    for (int j = i+1; j < buffer_count; j++) {
      if (buffer[i].time > buffer[j].time) {
        Record tmp = buffer[i];
        buffer[i] = buffer[j];
        buffer[j] = tmp;
      }
    }
  }

  //--- 開新檔寫入 ---
  File dst = SD.open("temp.tmp", FILE_WRITE);
  if (!dst) { src.close(); return; }
  dst.println("Timestamp,Temperature_C,Humidity_%");

  // 寫入緩衝
  for (int i = 0; i < buffer_count; i++) {
    char ts[20];
    sprintf(ts, "%04d-%02d-%02d %02d:%02d:00",
            buffer[i].time.year(), buffer[i].time.month(), buffer[i].time.day(),
            buffer[i].time.hour(), buffer[i].time.minute());
    dst.print(ts); dst.print(",");
    dst.print(buffer[i].temp, 1); dst.print(",");
    dst.println((int)buffer[i].hum);
  }

  //--- 其餘資料插入排序 ---
  while (src.available()) {
    String line = src.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    char *tok = strtok((char*)line.c_str(), ",");
    if (!tok) continue;
    int y,mo,d,hr,mi;
    if (sscanf(tok, "%d-%d-%d %d:%d:00", &y,&mo,&d,&hr,&mi) != 5) continue;

    tok = strtok(NULL, ",");
    if (!tok) continue;
    float temp = atof(tok);
    tok = strtok(NULL, ",");
    if (!tok) continue;
    float hum = atof(tok);

    DateTime dt(y,mo,d,hr,mi,0);
    bool inserted = false;
    for (int i = 0; i < buffer_count; i++) {
      if (dt < buffer[i].time) {
        if (buffer_count < BUFFER_SIZE) buffer_count++;
        for (int j = buffer_count-1; j > i; j--) buffer[j] = buffer[j-1];
        buffer[i] = {dt, temp, hum};
        inserted = true;
        break;
      }
    }
    if (!inserted && buffer_count < BUFFER_SIZE) {
      buffer[buffer_count++] = {dt, temp, hum};
    }

    if (buffer_count == BUFFER_SIZE) {
      for (int i = 0; i < BUFFER_SIZE; i++) {
        char ts[20];
        sprintf(ts, "%04d-%02d-%02d %02d:%02d:00",
                buffer[i].time.year(), buffer[i].time.month(), buffer[i].time.day(),
                buffer[i].time.hour(), buffer[i].time.minute());
        dst.print(ts); dst.print(",");
        dst.print(buffer[i].temp, 1); dst.print(",");
        dst.println((int)buffer[i].hum);
      }
      buffer_count = 0;
    }
  }

  //--- 寫入剩餘緩衝 ---
  for (int i = 0; i < buffer_count; i++) {
    char ts[20];
    sprintf(ts, "%04d-%02d-%02d %02d:%02d:00",
            buffer[i].time.year(), buffer[i].time.month(), buffer[i].time.day(),
            buffer[i].time.hour(), buffer[i].time.minute());
    dst.print(ts); dst.print(",");
    dst.print(buffer[i].temp, 1); dst.print(",");
    dst.println((int)buffer[i].hum);
  }

  src.close(); dst.close();

  //--- 複製回原檔 ---
  SD.remove(FILENAME);
  src = SD.open("temp.tmp", FILE_READ);
  dst = SD.open(FILENAME, FILE_WRITE);
  if (src && dst) {
    while (src.available()) dst.write(src.read());
    dst.close(); src.close();
    SD.remove("temp.tmp");
  }
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

  File file = SD.open(FILENAME);
  if (!file) {
    Serial.println("無法開啟 temp.csv");
    return;
  }

  file.readStringUntil('\n'); // 跳過 header

  // 計算總筆數
  int total_lines = 0;
  while (file.available()) {
    if (file.readStringUntil('\n').length() > 0) total_lines++;
  }
  file.close();

  if (total_lines == 0) {
    Serial.println("無資料");
    return;
  }

  int skip_lines = max(0, total_lines - MAX_POINTS);
  file = SD.open(FILENAME);
  file.readStringUntil('\n');

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

  char line[96];
  while (file.available() && index < MAX_POINTS) {
    size_t len = file.readBytesUntil('\n', line, sizeof(line) - 1);
    if (len == 0) break;
    line[len] = '\0';

    // // === 關鍵 Debug：印出原始資料 ===
    // Serial.print("RAW [");
    // Serial.print(index);
    // Serial.print("]: ");
    // Serial.println(line);

    // === 解析時間 ===
    char* token = strtok(line, ",");
    if (!token) {
      // Serial.println("  解析失敗：無時間");
      continue;
    }
    // Serial.print("  時間: "); Serial.println(token);

    int y, mo, d, hr, mi;
    if (sscanf(token, "%d-%d-%d %d:%d:00", &y, &mo, &d, &hr, &mi) != 5) {
      // Serial.println("  時間格式錯誤");
      continue;
    }
    DateTime record_time(y, mo, d, hr, mi, 0);

    // === 解析溫度 ===
    token = strtok(NULL, ",");
    if (!token) {
      // Serial.println("  無溫度欄位");
      continue;
    }
    // Serial.print("  溫度字串: ["); Serial.print(token); Serial.println("]");

    // 清理空白
    char* clean = token;
    while (*clean == ' ') clean++;
    char* end = clean + strlen(clean) - 1;
    while (end > clean && (*end == ' ' || *end == '\r' || *end == '\n')) *end-- = '\0';

    float t = atof(clean);
    // Serial.print("  解析溫度: "); Serial.println(t, 3);  // 印 3 位小數

    // === 解析濕度 ===
    token = strtok(NULL, ",");
    if (!token) {
      // Serial.println("  無濕度欄位");
      continue;
    }
    clean = token;
    while (*clean == ' ') clean++;
    end = clean + strlen(clean) - 1;
    while (end > clean && (*end == ' ' || *end == '\r' || *end == '\n')) *end-- = '\0';
    float h = atof(clean);
    // Serial.print("  濕度: "); Serial.println(h, 1);

    // === 繪圖 ===
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
  file.close();

  // 畫點 + 刻度（不變）
  if (last_x >= 0) {
    mylcd.Set_Draw_color(YELLOW); mylcd.Fill_Circle(last_x, last_temp_y, 2);
    mylcd.Set_Draw_color(CYAN);   mylcd.Fill_Circle(last_x, last_hum_y, 2);
  }

  mylcd.Set_Draw_color(BLACK);
  mylcd.Fill_Rectangle(GRAPH_X, GRAPH_BOTTOM + 6, GRAPH_X + GRAPH_W, GRAPH_BOTTOM + 20);

  mylcd.Set_Text_Size(2);
  mylcd.Set_Text_colour(WHITE);
  mylcd.Set_Text_Back_colour(BLACK);
  for (int i = 0; i < tick_index; i++) {
    mylcd.Draw_Fast_VLine(ticks[i].x, GRAPH_BOTTOM, 5);
    char buf[6]; sprintf(buf, "%02d:%02d", ticks[i].time.hour(), ticks[i].time.minute());
    printWithBackground(buf, ticks[i].x - 12, GRAPH_BOTTOM + 8, WHITE, BLACK, 2);
  }
}


void printWithBackground(const char* s, int x, int y, uint16_t textColor, uint16_t bgColor, uint8_t text_size) {
  int char_w = 6 * text_size; // 每個字元寬度（根據 Set_Text_Size(2)）
  int char_h = 8 * text_size; // 每個字元高度（根據 Set_Text_Size(2)）
  int text_w = strlen(s) * char_w;

  mylcd.Set_Text_Size(text_size);
  mylcd.Set_Draw_color(bgColor);
  mylcd.Fill_Rectangle(x, y, x + text_w, y + char_h);

  mylcd.Set_Text_colour(textColor);
  mylcd.Set_Text_Back_colour(bgColor); // ✅ 加上這行，確保文字底色一致
  mylcd.Print_String(s, x, y);

}
