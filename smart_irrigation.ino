#define BLYNK_TEMPLATE_ID "TMPL6mRR29z4_"
#define BLYNK_TEMPLATE_NAME "cambiendoam"
#define BLYNK_AUTH_TOKEN "LBCnvrzO68B7MmnYD4wGTGp302b3Kb9U"
// ====== Includes ======

#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <driver/adc.h>
// ====== Wi-Fi Config ======
const char* ssid     = "CP";
const char* password = "d26012004#";

// ====== LCD Config ======
#define I2C_SDA 4
#define I2C_SCL 5
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ====== PIN Config ======
const int soilPin      = 2;   // ADC soil moisture
const int relayPin     = 3;   // Relay (active LOW)
const int ledPin       = 7;   // LED indicator
const int interruptPin = 10;  // Button
const int buzzerPin    = 6;   // Buzzer
const int waterPin     = 1;   // ADC water level

// ====== Control Parameters ======
int TARGET_MOISTURE = 30; // ban đầu 30%
const float HYST            = 6.0f;  // tạo ra 1 ngưỡng +_6%/2
const unsigned long MAX_WATERING_TIME = 15000UL; // chạy chế dộ thủ công 15s
const int WET_RAW = 3900;
const int DRY_RAW = 4095;
const unsigned long DEBOUNCE_DELAY = 50; // thời gian chống dội nút 

volatile bool soilTickFlag = false;// thông báo thành true khi bắt đầu lấy độ ẩm
volatile bool buttonPressedFlag = false;// thay đổi thành true khi nhấn nút 
// quản lý trạng thái hệ thống
bool  isManualMode  = false;//xác định chế độ  true =thủ công  false= tự động
bool  pumpState     = false;//dùng cho bơm  true =bơm bật  false =bơm tắt 
bool  waterLow      = false;// dùng trong mực nước  true=cảnh báo ,còi false=bth
//Biến dữ liệu cảm biến 
unsigned long wateringStart = 0;
//dùng để ghi lại thời gian bắt đầu nhấn nút rồi manual rồi so sánh với 15s đó 
float currentMoisture = 0.0f;//Lưu trữ giá trị độ ẩm đất hiện tại 
int   waterPercent = 0;//lưu trữ phần trăm mực nước
float onThreshold = 0.0f;// định nghĩa ngưỡng thấp cho độ ẩm ->bật bơm 
float offThreshold = 0.0f;// định nghĩa ngưỡng cao cho độ ẩm ->tắt bơm 
unsigned long lastButtonPress = 0; // thời gian khi dùng nút nhấn 
unsigned long countdownStart = 0; // For countdown display

hw_timer_t* soilTimer = nullptr;//khai báo phần timer của độ ẩm 

// CHỈNH SỬA: logic BẬT/TẮT cho Active LOW Relay
#define PUMP_ON()  digitalWrite(relayPin, HIGH)
#define PUMP_OFF() digitalWrite(relayPin, LOW)


//giới hạn từ 0 ->100
float clamp100(float v) {
  return v < 0.0f ? 0.0f : (v > 100.0f ? 100.0f : v);
}
// đọc giá trị độ ẩm 
float adcToMoisture(int raw) {
  if (raw <= WET_RAW) return 100.0f;
  if (raw >= DRY_RAW) return 0.0f;
  return clamp100(100.0f * (float)(DRY_RAW - raw) / (float)(DRY_RAW - WET_RAW));
}
// setup hiển thị đảm bảo chất lượng trên LCD
void lcdPrint16(uint8_t row, const char* text) {
  char buf[17]; size_t n = 0;
  while (text[n] && n < 16) { buf[n] = text[n]; n++; }
  while (n < 16) { buf[n++] = ' '; }
  buf[16] = '\0';
  lcd.setCursor(0, row);
  lcd.print(buf);
}
//đọc giá trị độ ẩm đất chống nhiễu 8 lần để tính trung bình
int readSoilADC_Avg() {
  long sum = 0;
  for (int i = 0; i < 8; ++i) {
    sum += analogRead(soilPin);
    ets_delay_us(200);//độ trễ micro
  }
  return (int)(sum / 8);
}
//đọc giá trị lượng nước chống nhiễu 8 lần để tính trung bình
int readWaterADC_Avg() {
  long sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogRead(waterPin);
    delay(10);
  }
  return sum / 8;
}
// chuyển giá trị của ADC sang %
void updateWaterLevel() {
  int raw = readWaterADC_Avg();
  waterPercent = map(raw, 500, 2011, 0, 100);
  waterPercent = constrain(waterPercent, 0, 100);
}

// hàm ngắt interrupt
void IRAM_ATTR onButtonISR() { 
  buttonPressedFlag = true;
}
// tạo ra cơ chế timer
void IRAM_ATTR soilTimerISR() { soilTickFlag = true; }

// ====== Blynk Handlers ======
BLYNK_WRITE(V3) {
  if (param.asInt() == 1) {
    isManualMode = !isManualMode;
    if (isManualMode) {
      pumpState = true;
      wateringStart = millis();
      countdownStart = millis();
      Serial.println("→ MANUAL MODE via Blynk");
    } else {
      pumpState = false;
      wateringStart = 0;
      countdownStart = 0;
      Serial.println("→ AUTO MODE via Blynk");
    }
  }
}

BLYNK_WRITE(V4) {
  String mode = param.asStr();
  if (mode == "AUTO" && isManualMode) {
    isManualMode = false; 
    pumpState = false;
    wateringStart = 0;
    countdownStart = 0;
  } else if (mode == "MANUAL" && !isManualMode) {
    isManualMode = true; 
    pumpState = true; 
    wateringStart = millis();
    countdownStart = millis();
  }
}
BLYNK_WRITE(V5) {
  int newTarget = param.asInt();
  if (newTarget >= 0 && newTarget <= 100) {
    // Cập nhật độ ẩm mục tiêu
    TARGET_MOISTURE = newTarget;
    onThreshold  = TARGET_MOISTURE - (HYST / 2.0f);
    offThreshold = TARGET_MOISTURE + (HYST / 2.0f);
    Serial.printf(" Target moisture updated: %d%% (ON=%.1f, OFF=%.1f)\n", TARGET_MOISTURE, onThreshold, offThreshold);
    
    // CHỈNH SỬA: Đồng bộ lại giá trị widget V5
    Blynk.virtualWrite(V5, TARGET_MOISTURE);
  }
}


// ====== SETUP ======
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("=== ESP32-C3 Smart Irrigation System ===");

  pinMode(relayPin, OUTPUT);
  pinMode(ledPin, OUTPUT);
  pinMode(interruptPin, INPUT_PULLUP);
  pinMode(soilPin, INPUT);
  pinMode(buzzerPin, OUTPUT);
  pinMode(waterPin, INPUT);
  PUMP_OFF(); // Đảm bảo bơm tắt khi khởi động (HIGH nếu là active LOW)

  // LCD
  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcdPrint16(0, "Soil Monitor");
  lcdPrint16(1, "Initializing...");
analogSetAttenuation(ADC_11db);
  // Interrupt button
  attachInterrupt(digitalPinToInterrupt(interruptPin), onButtonISR, FALLING);

  // Moisture thresholds
  onThreshold  = TARGET_MOISTURE - (HYST / 2.0f);
  offThreshold = TARGET_MOISTURE + (HYST / 2.0f);
  Serial.printf("Target=%d%% | ON=%.1f | OFF=%.1f\n", TARGET_MOISTURE, onThreshold, offThreshold);

  // Hardware Timer (5s) timer và timer interrupt
  soilTimer = timerBegin(0, 80, true);
  timerAttachInterrupt(soilTimer, &soilTimerISR, true);
  timerAlarmWrite(soilTimer, 5000000ULL, true);
  timerAlarmEnable(soilTimer);

  // ====== Non-blocking Wi-Fi + Blynk ======
  Serial.println("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  int wifi_attempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifi_attempts < 20) {
    delay(500);
    Serial.print(".");
    wifi_attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n WiFi Connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n WiFi failed, running offline...");
  }

  Serial.println("Connecting to WiFi & Blynk...");
  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, password);
  while (!Blynk.connected()) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\n Blynk Connected!");

  // CHỈNH SỬA: Gửi giá trị mục tiêu lên Blynk sau khi kết nối
  Blynk.virtualWrite(V5, TARGET_MOISTURE);

  lcdPrint16(0, "Hum:    % PMP:OFF");
  lcdPrint16(1, "MODE: AUTO");
}

// ====== LOOP ======
void loop() {
  Blynk.run();
int n= analogRead(2);
Serial.println(n);
  // kết nối tới Wi-Fi / Blynk chu kì 10s
  static unsigned long lastReconnect = 0;
  if (millis() - lastReconnect > 10000) {
    lastReconnect = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println(" WiFi lost — reconnecting...");
      WiFi.begin(ssid, password);
    } else if (!Blynk.connected()) {
      Serial.println("Reconnecting Blynk...");
      Blynk.connect(1000);
    }
  }

  // Water level
  updateWaterLevel();
// cảnh báo mực nước dưới 10 %
  if (waterPercent < 10) {
    if (!waterLow) {
      waterLow = true;
      Serial.println("WATER LOW → Pump disabled!");
      Blynk.logEvent("canhbao", "Bình nước đã hết!");
    }
    digitalWrite(buzzerPin, HIGH);
    PUMP_OFF();
  } else {
    waterLow = false;
    digitalWrite(buzzerPin, LOW);
  }

  // Button khi được nhấn interrupt
  if (buttonPressedFlag) {
    noInterrupts();//khi nhấn nút thì khóa lại không cho các việc khác ảnh hưởng
    buttonPressedFlag = false;
    interrupts();// mở ra lại 
    
    // Chống dội nút thực hiện trong loop()
    if (millis() - lastButtonPress > DEBOUNCE_DELAY) { 
      if (digitalRead(interruptPin) == LOW) { // Verify button state
        isManualMode = !isManualMode;
        if (isManualMode) {
          pumpState = true;
          wateringStart = millis();
          countdownStart = millis();
          Serial.println("→ ENTER MANUAL MODE (Button)");
        } else {
          pumpState = false;
          wateringStart = 0;
          countdownStart = 0;
          Serial.println("→ EXIT MANUAL MODE (Button)");
        }
      }
      lastButtonPress = millis(); // Cập nhật lần nhấn cuối cùng
    }
  }

  // Soil sampling
  if (soilTickFlag) {
    soilTickFlag = false;
    int raw = readSoilADC_Avg();
    currentMoisture = adcToMoisture(raw);
    Serial.printf("[TIMER] Soil: %.1f%%\n", currentMoisture);
  }

  // điều khiển bơm 
  if (waterLow) {
    pumpState = false;
  } else if (isManualMode) {// chế độ thủ công 
    if (millis() - wateringStart > MAX_WATERING_TIME) {
      isManualMode = false;
      pumpState = false;
      wateringStart = 0;
      countdownStart = 0;
      Serial.println("Manual watering finished → AUTO");
    } else {
      pumpState = true;
    }
  } else {//chế độ tự động
    if (!pumpState && currentMoisture < onThreshold) pumpState = true;
    else if (pumpState && currentMoisture > offThreshold) pumpState = false;
  }

  if (pumpState) PUMP_ON(); else PUMP_OFF();
  digitalWrite(ledPin, pumpState ? HIGH : LOW);

  // LCD update (0.5)
  static unsigned long lastLcd = 0;
  if (millis() - lastLcd > 500) {
    char line1[17], line2[17];
    if (isManualMode && countdownStart > 0) {
      unsigned long elapsed = millis() - countdownStart;
      int secondsLeft = (MAX_WATERING_TIME - elapsed) / 1000;
      if (secondsLeft < 0) secondsLeft = 0;
      snprintf(line1, sizeof(line1), "Hum:%3d%% PMP:%s", (int)currentMoisture, pumpState ? "ON " : "OFF");
      snprintf(line2, sizeof(line2), "MAN:%2ds WL:%2d%%", secondsLeft, waterPercent);
    } else {
      snprintf(line1, sizeof(line1), "Hum:%3d%% PMP:%s", (int)currentMoisture, pumpState ? "ON " : "OFF");
      snprintf(line2, sizeof(line2), "Mod:%-4s WL:%3d%%", isManualMode ? "MANUAL" : "AUTO", waterPercent);
    }
    lcdPrint16(0, line1);
    lcdPrint16(1, line2);
    lastLcd = millis();
  }

  // Blynk update (2s)
  static unsigned long lastBlynk = 0;
  if (millis() - lastBlynk > 2000) {
    lastBlynk = millis();
    Blynk.virtualWrite(V0, currentMoisture);
    Blynk.virtualWrite(V1, waterPercent);
    Blynk.virtualWrite(V2, pumpState);
    Blynk.virtualWrite(V4, isManualMode ? "MANUAL" : "AUTO");
  }

  delay(30);
}