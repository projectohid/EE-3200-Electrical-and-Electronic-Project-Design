#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <time.h>

// ====== USER CONFIG ======
#define WIFI_SSID "No internet"
#define WIFI_PASSWORD "improvedll"

#define API_KEY "AIzaSyBJkfaMqeF4VOD1RKiSaEQoSLjlRGT2UKs"
#define DATABASE_URL "https://ecg-monitor-ohid-default-rtdb.asia-southeast1.firebasedatabase.app"

#define DEVICE_ID "esp32-ecg-01"
const int ECG_PIN = 34; // ADC1_CH6

// ====== Firebase ======
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ====== Sampling ======
const int SAMPLE_RATE = 250;   // Hz
const int CHUNK = SAMPLE_RATE; // 1-second chunks
int16_t buf[CHUNK];
int idxBuf = 0;

// Simple filters
float hp_y = 0.0f; // high-pass state
float hp_alpha;    // set in setup()
int16_t ma_window[5] = {0};
int ma_i = 0;

// Time
uint64_t epochMillisUTC()
{
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (uint64_t)tv.tv_sec * 1000ULL + (tv.tv_usec / 1000ULL);
}

void connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(300);
  }
}

void syncTime()
{
  configTime(0, 0, "pool.ntp.org", "time.nist.gov"); // UTC
  // wait for time
  for (int i = 0; i < 30; ++i)
  {
    time_t now = time(nullptr);
    if (now > 1700000000)
      return; // time synced
    delay(200);
  }
}

void setupFirebase()
{
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  Firebase.reconnectWiFi(true);

  // Anonymous sign-in (enable in Firebase Console)
  if (Firebase.signUp(&config, &auth, "", ""))
  {
    // ok
  }
  else
  {
    // still proceed; the library retries
  }
  Firebase.begin(&config, &auth);
}

void setup()
{
  // ADC config
  analogReadResolution(12);                   // 0..4095
  analogSetPinAttenuation(ECG_PIN, ADC_11db); // 0..3.3 V range

  connectWiFi();
  syncTime();
  setupFirebase();

  // High-pass single pole at ~0.5 Hz (baseline drift removal)
  float dt = 1.0f / SAMPLE_RATE;
  float rc = 1.0f / (2.0f * 3.1415926f * 0.5f);
  hp_alpha = rc / (rc + dt);
}

// 5-point moving average
int16_t movingAvg5(int16_t x)
{
  ma_window[ma_i] = x;
  ma_i = (ma_i + 1) % 5;
  long s = 0;
  for (int i = 0; i < 5; ++i)
    s += ma_window[i];
  return (int16_t)(s / 5);
}

void loop()
{
  static uint32_t nextUs = micros();
  const uint32_t periodUs = 1000000UL / SAMPLE_RATE; // 4000 us

  // pacing for exact 250 Hz
  int32_t delta = (int32_t)(micros() - nextUs);
  if (delta < 0)
    return; // not time yet
  nextUs += periodUs;

  // Read ADC
  int raw = analogRead(ECG_PIN); // 0..4095 counts

  // Convert to centered value (counts around mid-scale ~2048)
  int16_t centered = (int16_t)raw - 2048;

  // Moving average (noise smooth)
  int16_t ma = movingAvg5(centered);

  // High-pass (remove slow drift)
  hp_y = hp_alpha * (hp_y + ma - (int16_t)0);
  int16_t filtered = (int16_t)hp_y;

  // Save to buffer
  buf[idxBuf++] = filtered;

  // When full second collected -> upload
  if (idxBuf >= CHUNK)
  {
    uint64_t ts = epochMillisUTC();

    FirebaseJson json;
    json.set("sr", SAMPLE_RATE);
    json.set("unit", "counts_centered");
    json.set("ts", (double)ts); // store as number

    FirebaseJsonArray arr;
    for (int i = 0; i < CHUNK; ++i)
      arr.add((int)buf[i]);
    json.set("samples", arr);

    String path = String("/ecg/") + DEVICE_ID + "/" + String(ts);
    Firebase.RTDB.setJSON(&fbdo, path.c_str(), &json); // deterministic key

    idxBuf = 0; // reset buffer
  }
}
