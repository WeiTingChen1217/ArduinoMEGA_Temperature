#include <LCDWIKI_GUI.h>
#include <LCDWIKI_KBV.h>
#include <DHT.h>
#include <SD.h>
#include <SPI.h>
#include <RTClib.h>

LCDWIKI_KBV mylcd(ILI9481, 40, 38, 39, -1, 41);

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
unsigned long last_record = 0;
const long RECORD_INTERVAL = 60000;

void setup() {
  Serial.begin(9600);
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

  const char* compile_date = __DATE__;
  const char* compile_time = __TIME__;
  char s_month[6];
  int month = 1, day = 1, year = 2025, hour = 0, minute = 0, second = 0;
  static const char month_names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  if (sscanf(compile_date, "%5s %d %d", s_month, &day, &year) == 3) {
    char* p = strstr((char*)month_names, s_month);
    if (p) month = (p - month_names) / 3 + 1;
  }
  sscanf(compile_time, "%d:%d:%d", &hour, &minute, &second);
  start_time = DateTime(year, month, day, hour, minute, second);
  start_millis = millis();

  drawUI();
  writeCSVHeader();
  drawGraphFromSD();
}

DateTime getCurrentTime() {
  unsigned long elapsed = millis() - start_millis;
  return start_time + TimeSpan(elapsed / 1000);
}

void loop() {
  unsigned long now_millis = millis();
  DateTime now = getCurrentTime();

  if (now_millis - last_record >= RECORD_INTERVAL) {
    last_record = now_millis;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      logToSD(t, h, now);
      drawGraphFromSD();
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

void clearCSV() {
  SD.remove("temp.csv");
  File file = SD.open("temp.csv", FILE_WRITE);
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
  printWithBackground(temp_str, center2 - strlen(temp_str) * char_w / 2, 32, WHITE, BLACK, text_size);
  printWithBackground(hum_str, center3 - strlen(hum_str) * char_w / 2, 32, WHITE, BLACK, text_size);

  strcpy(last_time, time_str);
  strcpy(last_temp, temp_str);
  strcpy(last_hum, hum_str);
}

void writeCSVHeader() {
  File file = SD.open("temp.csv", FILE_WRITE);
  if (file && file.size() == 0) {
    file.println("Timestamp,Temperature_C,Humidity_%");
  }
  file.close();
}

void logToSD(float temp, float hum, DateTime now) {
  File file = SD.open("temp.csv", FILE_READ);
  if (!file) return;

  int line_count = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    if (line.length() > 0) line_count++;
  }
  file.close();

  if (line_count > MAX_RECORDS + 1) {
    File oldFile = SD.open("temp.csv", FILE_READ);
    String header = oldFile.readStringUntil('\n');
    String newContent = header + "\n";
    int skipped = 0;
    while (oldFile.available()) {
      String line = oldFile.readStringUntil('\n');
      if (skipped == 0) { skipped++; continue; }
      newContent += line + "\n";
    }
    oldFile.close();

    SD.remove("temp.csv");
    File newFile = SD.open("temp.csv", FILE_WRITE);
    newFile.print(newContent);
    newFile.close();
  }

  File fileWrite = SD.open("temp.csv", FILE_WRITE);
  if (fileWrite) {
    char timestamp[20];
    sprintf(timestamp, "%04d-%02d-%02d %02d:%02d:00",
            now.year(), now.month(), now.day(), now.hour(), now.minute());
    fileWrite.print(timestamp);
    fileWrite.print(",");
    fileWrite.print(temp, 1);
    fileWrite.print(",");
    fileWrite.println(hum, 0);
    fileWrite.close();
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
  const int TICK_COUNT = 4;
  const int MAX_POINTS = GRAPH_W; // 每筆資料對應 1px

  File file = SD.open("temp.csv");
  if (!file) {
    Serial.println("❌ 無法開啟 temp.csv");
    return;
  }

  file.readStringUntil('\n'); // 跳過標頭

  // 預掃總筆數
  int total_lines = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    if (line.length() > 0) total_lines++;
  }
  file.close();

  if (total_lines == 0) return;

  int skip_lines = max(0, total_lines - MAX_POINTS);

  file = SD.open("temp.csv");
  file.readStringUntil('\n'); // 跳過標頭

  // 跳過前面資料
  for (int i = 0; i < skip_lines; i++) {
    file.readStringUntil('\n');
  }

  mylcd.Set_Draw_color(BLACK);
  mylcd.Fill_Rectangle(GRAPH_X, GRAPH_Y, GRAPH_X + GRAPH_W, GRAPH_BOTTOM);

  float last_temp = 0, last_hum = 0;
  int last_x = -1, last_temp_y = -1, last_hum_y = -1;

  int index = 0;
  int tick_interval = MAX_POINTS / TICK_COUNT;

  struct Tick {
    int x;
    DateTime time;
  };
  Tick ticks[TICK_COUNT + 1];
  int tick_index = 0;

  char line[96];
  while (file.available() && index < MAX_POINTS) {
    size_t len = file.readBytesUntil('\n', line, sizeof(line) - 1);
    if (len == 0) break;
    line[len] = '\0';

    char* token = strtok(line, ",");
    if (!token) continue;
    char* timestamp = token;

    token = strtok(NULL, ",");
    if (!token) continue;
    float t = atof(token);

    token = strtok(NULL, ",");
    if (!token) continue;
    float h = atof(token);

    int y, mo, d, hr, mi;
    if (sscanf(timestamp, "%d-%d-%d %d:%d", &y, &mo, &d, &hr, &mi) != 5) continue;
    DateTime record_time(y, mo, d, hr, mi, 0);

    int x = GRAPH_X + index;
    int temp_y = tempToY(t);
    int hum_y = humToY(h);

    if (last_x >= 0) {
      mylcd.Set_Draw_color(YELLOW);
      mylcd.Draw_Line(last_x, last_temp_y, x, temp_y);
      mylcd.Set_Draw_color(CYAN);
      mylcd.Draw_Line(last_x, last_hum_y, x, hum_y);
    }

    last_x = x;
    last_temp_y = temp_y;
    last_hum_y = hum_y;

    if (tick_index <= TICK_COUNT && index % tick_interval == 0) {
      ticks[tick_index++] = {x, record_time};
    }

    index++;
  }

  file.close();

  // 畫最新資料點
  if (last_x >= 0) {
    mylcd.Set_Draw_color(YELLOW);
    mylcd.Fill_Circle(last_x, last_temp_y, 2);
    mylcd.Set_Draw_color(CYAN);
    mylcd.Fill_Circle(last_x, last_hum_y, 2);
  }

  // 清除 X 軸刻度區域
  mylcd.Set_Draw_color(BLACK);
  mylcd.Fill_Rectangle(GRAPH_X, GRAPH_BOTTOM + 6, GRAPH_X + GRAPH_W, GRAPH_BOTTOM + 20);

  // 繪製 X 軸刻度（以時間顯示）
  mylcd.Set_Text_colour(WHITE);
  mylcd.Set_Text_Back_colour(BLACK);
  mylcd.Set_Text_Size(2);
  for (int i = 0; i < tick_index; i++) {
    mylcd.Draw_Fast_VLine(ticks[i].x, GRAPH_BOTTOM, 5);
    char buf[6];
    sprintf(buf, "%02d:%02d", ticks[i].time.hour(), ticks[i].time.minute());
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
