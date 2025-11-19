#include <Arduino.h> // WAJIB untuk ESP32 core functions
#include <Wire.h> 
#include <WiFi.h> 
#include "RTClib.h" 
#include <LiquidCrystal_I2C.h> 
#include <Adafruit_NeoPixel.h> // LIBRARY BARU untuk Addressable LED

// --- Definisikan Pin I2C (RTC & LCD) ---
#define I2C_SDA_PIN 32 
#define I2C_SCL_PIN 33 

// --- Definisikan Pin Addressable LED ---
#define LED_PIN 13          // Pin tunggal untuk Addressable LED (Neopixel)
#define NUM_LEDS 1          // Asumsi hanya 1 LED RGB Addressable

// --- Definisikan Pin Sensor & Relay ---
#define TRIG_PIN 4      // HCSR04 Trigger
#define ECHO_PIN 5      // HCSR04 Echo
#define RELAY_MIST 25   // Relay 1: Mist Maker (Output Active LOW)
#define RELAY_FAN 26    // Relay 2: Fan (Output Active LOW)
#define BUZZER_PIN 17   // Buzzer Peringatan
#define BUTTON_PIN 34   // Tombol (Input Pull-up)

// --- Konstanta Pengukuran Air (dalam cm) ---
const float JARAK_PENUH = 2.0;    
const float JARAK_KOSONG = 10.0;  
const float RENTANG_AIR = JARAK_KOSONG - JARAK_PENUH;

// --- Konstanta Kontrol Waktu & Mode ---
const int FOCUS_START_1_HOUR = 8;   // 08:00 - 11:59
const int FOCUS_END_1_HOUR = 12;    
const int FOCUS_START_2_HOUR = 18;  // 18:00 - 19:59
const int FOCUS_END_2_HOUR = 20;    
const int RELAX_START_HOUR = 21;    // 21:00 - 23:59
const int RELAX_END_HOUR = 24;      

const long FAN_ON_DURATION = 3000;      
const long FAN_CYCLE_TIME = 120000;     
const long DEBOUNCE_DELAY = 100;        
const long COLOR_CYCLE_TIME = 5000;     // Ganti warna relax mode setiap 5 detik

// --- Objek Library ---
RTC_DS3231 rtc;
LiquidCrystal_I2C lcd(0x27, 16, 2); 
Adafruit_NeoPixel pixels(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// --- Variabel State Global ---
bool mistMakerStatus = false;       
bool isLowWater = false;            
bool isAutoMode = true;             
unsigned long lastFanToggleTime = 0;
unsigned long lastButtonPressTime = 0;
unsigned long lastColorChangeTime = 0;
int currentColorIndex = 0;

// ==============================================================================
//                              FUNGSI KONTROL RGB (NEOPIXEL)
// ==============================================================================

/**
 * @brief Mengatur warna LED Addressable.
 * @param r Intensitas Merah (0-255)
 * @param g Intensitas Hijau (0-255)
 * @param b Intensitas Biru (0-255)
 */
void setRGBColor(int r, int g, int b) {
  pixels.setPixelColor(0, pixels.Color(r, g, b));
  pixels.show(); // Kirim data ke LED
}

/**
 * @brief Mengatur siklus warna lembut untuk Mode Relax.
 */
void handleRelaxColorCycle() {
  // Warna-warna tenang (Ungu, Biru, Teal, Biru Muda)
  const uint32_t colors[] = {
    pixels.Color(128, 0, 128), // Ungu
    pixels.Color(0, 0, 255),   // Biru
    pixels.Color(0, 128, 128), // Teal
    pixels.Color(173, 216, 230) // Biru Muda
  };
  const int numColors = sizeof(colors) / sizeof(colors[0]);

  if (millis() - lastColorChangeTime >= COLOR_CYCLE_TIME) {
    lastColorChangeTime = millis();
    currentColorIndex = (currentColorIndex + 1) % numColors;
  }

  pixels.setPixelColor(0, colors[currentColorIndex]);
  pixels.show();
}

// ==============================================================================
//                              FUNGSI RTC & OTOMASI
// ==============================================================================

bool checkAutoMode() {
  DateTime now = rtc.now(); 
  int currentHour = now.hour();
  
  bool isFocusTime1 = (currentHour >= FOCUS_START_1_HOUR && currentHour < FOCUS_END_1_HOUR);
  bool isFocusTime2 = (currentHour >= FOCUS_START_2_HOUR && currentHour < FOCUS_END_2_HOUR);
  bool isRelaxTime = (currentHour >= RELAX_START_HOUR && currentHour < RELAX_END_HOUR);

  return isFocusTime1 || isFocusTime2 || isRelaxTime; 
}

// ==============================================================================
//                              FUNGSI SENSOR & KONTROL
// ==============================================================================

float measureDistance() {
  float totalDistance = 0;
  int numReadings = 5;
  for (int i = 0; i < numReadings; i++) {
    digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    long duration = pulseIn(ECHO_PIN, HIGH);
    float distanceCm = duration * 0.034 / 2;
    totalDistance += distanceCm;
    delay(10); 
  }
  return totalDistance / numReadings;
}

void controlDevices() {
  bool desiredMistState = false; 

  if (isAutoMode) {
    desiredMistState = checkAutoMode(); 
  } else {
    desiredMistState = mistMakerStatus; 
  }

  // --- Kontrol Safety Lock ---
  if (isLowWater) {
    digitalWrite(RELAY_MIST, HIGH); 
    digitalWrite(RELAY_FAN, HIGH);  
    return;
  } 
  
  mistMakerStatus = desiredMistState; 

  // Eksekusi Kontrol Relay Mist Maker (Active LOW)
  digitalWrite(RELAY_MIST, desiredMistState ? LOW : HIGH); 

  // Kontrol Kipas Intermiten
  if (desiredMistState) {
    if (millis() - lastFanToggleTime >= FAN_CYCLE_TIME) {
      lastFanToggleTime = millis(); 
    }
    
    if (millis() - lastFanToggleTime <= FAN_ON_DURATION) {
      digitalWrite(RELAY_FAN, LOW); // FAN ON
    } else {
      digitalWrite(RELAY_FAN, HIGH); // FAN OFF
    }
  } else {
    digitalWrite(RELAY_FAN, HIGH); // Mist Maker OFF, FAN juga OFF
  }
}

void updateDisplayAndLED(float distance) {
  float persentase = 100.0 * (JARAK_KOSONG - distance) / RENTANG_AIR;
  if (persentase < 0) persentase = 0;
  if (persentase > 100) persentase = 100;
  
  // Tentukan status isLowWater terlebih dahulu
  isLowWater = (persentase <= 5); 
  
  // --- Tentukan Warna RGB Berdasarkan Status & Mode ---
  
  // 1. Logika Low Water Level (Paling Prioritas)
  if (isLowWater) { 
    setRGBColor(255, 0, 0); // Merah Solid
    mistMakerStatus = false; // Paksa Mist Maker mati
  } 
  // 2. Logika Mode Otomatis
  else if (isAutoMode) {
    bool isMistActive = checkAutoMode();
    if (isMistActive) {
      DateTime now = rtc.now();
      int currentHour = now.hour();

      if (currentHour >= RELAX_START_HOUR && currentHour < RELAX_END_HOUR) {
        handleRelaxColorCycle(); // Warna Berpindah Tenang (Ungu, Biru, dll.)
      } else {
        setRGBColor(255, 120, 0); // Orange (Focus Mode)
      }
    } else {
      setRGBColor(0, 0, 0); // OFF (Standby di luar jam otomatis)
    }
  } 
  // 3. Logika Mode Manual
  else {
    if (mistMakerStatus) {
      setRGBColor(0, 255, 0); // Hijau (Mist ON)
    } else {
      setRGBColor(0, 0, 0); // OFF (Mist OFF)
    }
    
    // Peringatan level air rendah di Mode Manual (jika di atas level LowWater Kritis 5%)
    if (persentase <= 30) {
      setRGBColor(255, 100, 0); // Orange Peringatan
    }
  }

  // --- Logika Buzzer ---
  digitalWrite(BUZZER_PIN, LOW); // Default mati
  if (isLowWater) {
     // Buzzer Berkedip
    if (millis() % 300 < 150) { 
       digitalWrite(BUZZER_PIN, HIGH);
    }
  } else if (persentase <= 30) {
     // Buzzer Beep pelan
    if (millis() % 5000 < 100) { 
       digitalWrite(BUZZER_PIN, HIGH);
    }
  }


  // --- Update LCD ---
  lcd.clear();
  DateTime now = rtc.now();
  
  const char* modeStr = isAutoMode ? "Auto" : "Manual";
  const char* statusStr = mistMakerStatus ? "ON" : "OFF";
  
  lcd.setCursor(0, 0);
  lcd.printf("Jam: %02d:%02d | %s", now.hour(), now.minute(), modeStr); 

  lcd.setCursor(0, 1);
  lcd.printf("Air: %3.0f%% | Status: %s", persentase, statusStr);
}

void handleButton() {
  unsigned long currentTime = millis();
  
  if (digitalRead(BUTTON_PIN) == LOW) {
    if (currentTime - lastButtonPressTime > DEBOUNCE_DELAY) {
      
      isAutoMode = !isAutoMode; 
      Serial.printf("Mode diubah ke: %s\n", isAutoMode ? "Otomatis" : "Manual");

      if (!isAutoMode) {
        mistMakerStatus = false;
      }
      
      lastButtonPressTime = currentTime; 
    }
  }
}

// ==============================================================================
//                              SETUP DAN LOOP
// ==============================================================================

void setup() {
  Serial.begin(115200);

  // --- Inisialisasi Pin OUTPUT/INPUT ---
  pinMode(RELAY_MIST, OUTPUT); pinMode(RELAY_FAN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT); pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT); pinMode(BUTTON_PIN, INPUT_PULLUP); 

  // --- INISIALISASI Addressable LED (NeoPixel) ---
  pixels.begin(); // Inisialisasi library NeoPixel
  pixels.clear(); // Matikan semua LED saat startup
  pixels.show();  // Tampilkan perubahan (matikan)

  // --- INISIALISASI I2C KUSTOM ---
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN); 

  // --- Inisialisasi LCD & RTC ---
  lcd.init();
  lcd.backlight();
  lcd.print("AromaSync By");
  lcd.setCursor(0, 1);
  lcd.print("SERENTECH");
  
  if (!rtc.begin()) {
    Serial.println("RTC TIDAK DITEMUKAN. Cek I2C Pin 32/33.");
  }
  
  // PENTING: Hapus komentar pada baris di bawah ini sekali untuk mengatur waktu RTC
  // rtc.adjust(DateTime(F(__DATE__), F(__TIME__))); 

  // Pastikan semua perangkat mati saat startup
  digitalWrite(RELAY_MIST, HIGH); 
  digitalWrite(RELAY_FAN, HIGH);  
  digitalWrite(BUZZER_PIN, LOW);
}

void loop() {
  // 1. Cek Tombol (mengganti mode)
  handleButton(); 
  
  // 2. Kontrol Perangkat (Mencakup Logic Otomatis RTC)
  controlDevices();
  
  // 3. Ukur Jarak dan Update Tampilan/LED
  float distance = measureDistance();
  updateDisplayAndLED(distance);
  
  delay(100); 
}