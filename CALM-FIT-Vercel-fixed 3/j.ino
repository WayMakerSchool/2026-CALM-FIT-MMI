/*
 * ============================================================
 * CALM FIT FINAL SENSOR + WIFI
 *
 * XIAO ESP32-S3 Sense
 * MAX30102
 * GY-61
 * Built-in PDM Microphone
 * Edge Impulse Voice AI
 * Wi-Fi HTTP Telemetry
 *
 * ============================================================
 *
 * PIN
 *
 * MAX30102
 * SDA -> D4 / GPIO5
 * SCL -> D5 / GPIO6
 *
 * GY-61
 * X -> D0 / GPIO1
 * Y -> D1 / GPIO2
 * Z -> D2 / GPIO3
 *
 * XIAO ESP32-S3 Sense Microphone
 * PDM CLK  -> GPIO42
 * PDM DATA -> GPIO41
 *
 * Serial Monitor
 * 115200
 *
 * ============================================================
 */

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>

#include "MAX30105.h"
#include "heartRate.h"

#include <ESP_I2S.h>
#include <ASD_Voice_Detector_inferencing.h>


// ============================================================
// Wi-Fi 설정
// ============================================================

#define CONTEST_MODE


#ifdef CONTEST_MODE

// 등록된 Wi-Fi 중 근처에서 신호가 잡히는 곳에 자동으로 연결됩니다. (2.4GHz만 가능)
// 장소가 바뀌어도 코드를 고칠 필요 없이, 여기에 Wi-Fi를 추가하면 됩니다.
struct WifiNetwork {
  const char* ssid;
  const char* password;
};

const WifiNetwork WIFI_NETWORKS[] = {
 // { "SK_WiFiGIGA3994_2.4G", "DUN44@8525" },   // 집
  { "WMS", "WMS1348B2F" },   // 학교
 // { "SK_8534_2.4G", "CAP3F@3696" },
};
const char* SERVER_URL = 
"https://calm-fit-vercel-hobby-ready-1.vercel.app/api/telemetry";
  //"https://calm-fit-vercel-hobby-ready-1-6mqm52qo1-mmi-9a34.vercel.app/api/telemetry";
#endif

WiFiMulti wifiMulti;


// ============================================================
// Device
// ============================================================

const char* DEVICE_ID =
  "CALM-FIT-01";


// ⚠️ 여기에 Vercel의 CALMFIT_DEVICE_TOKEN을 그대로 넣으세요.
// ⚠️ Vercel 값과 글자 하나까지 같아야 합니다.
const char* DEVICE_TOKEN =
  "92a4650f54a29a91450091850bf8276240524c2bba4c3af6384ba25f628cb637";


// ============================================================
// MAX30102
// ============================================================

MAX30105 particleSensor;

bool heartSensorReady =
  false;

long lastBeat =
  0;

float bpm =
  0.0;

// 튀는 값 하나에 단계가 묶이지 않도록 최근 박동 4개를 평균
const int BPM_AVERAGE_COUNT =
  4;

float bpmHistory[BPM_AVERAGE_COUNT] =
  { 0 };

int bpmHistoryCount =
  0;

unsigned long lastValidBeatAt =
  0;

// 이 시간 동안 새 박동이 없으면 예전 심박을 버리고 측정 대기(0)로 돌아감
const unsigned long BPM_EXPIRE_MS =
  5000;

long irValue =
  0;

bool fingerDetected =
  false;

long irNoFingerBaseline =
  0;

long fingerOnThreshold =
  10000;

unsigned long lastHeartPrint =
  0;


// ============================================================
// GY-61
// ============================================================

#define GY61_X D0
#define GY61_Y D1
#define GY61_Z D2

int baseX =
  0;

int baseY =
  0;

int baseZ =
  0;

int prevX =
  0;

int prevY =
  0;

int prevZ =
  0;

int noiseFloor =
  3;

int rawMotion =
  0;

int motionScore =
  0;

String motionLevel =
  "NORMAL";

// ============================================================
// ⭐ [v4] 움직임 단계 판정
//
// 올라갈 때는 천천히: 흔들기 기준값(20점 / 60점)은 그대로 두고,
//                    그 이상으로 몇 초 동안 '계속' 움직여야 단계가 올라감
// 내려갈 때는 빠르게: 가만히 있으면(완전히 멈추지 않아도) 1초 안에 바로 내려감
//
// 아래 숫자만 바꾸면 민감도를 조절할 수 있습니다.
// ============================================================

// [올라가기] BIG MOVE(20점 이상)가 이 초 동안 계속되면 → 웹사이트 '주의'
const int CAUTION_HOLD_SEC =
  3;

// [올라가기] BIG BIG MOVE(60점 이상)가 이 초 동안 계속되면 → 웹사이트 '위험'
const int DANGER_HOLD_SEC =
  5;

// [내려가기] 움직임이 이 점수 '미만'이면 가만히 있는 것으로 봄
// (완전히 멈추지 않고 조금 꼼지락거려도 이 점수 아래면 '가만히'로 인정)
// 서 있어도 잘 안 내려가면 25~30으로 올리세요.
const int CALM_MOTION_SCORE =
  20;

// [내려가기] 가만히 있는 상태가 이 시간(ms) 이어지면 바로 '안정'
// (60점 이상 큰 움직임만 이 시간 동안 없으면 '위험' → '주의')
// 흔드는 도중에 잘못 내려가면 1000으로 늘리세요.
const unsigned long CALM_HOLD_MS =
  700;


// 1초마다 '그 1초 동안의 가장 큰 움직임 점수'를 기록 (최근 10초)
const int MOTION_HISTORY_SEC =
  10;

int motionSecMax[MOTION_HISTORY_SEC] =
  { 0 };

int motionSecCount =
  0;

int motionSecHead =
  0;

unsigned long motionBinStart =
  0;

int motionBinMax =
  -1;   // -1: 이번 1초 동안 아직 측정 없음

// 마지막으로 CALM_MOTION_SCORE 이상 / 60점 이상 움직였던 시각
unsigned long lastAboveCalmAt =
  0;

unsigned long lastAboveDangerAt =
  0;

unsigned long lastMotionUpdateAt =
  0;

// 웹사이트로 보내는 단계용 움직임 (0: NORMAL, 1: BIG MOVE, 2: BIG BIG MOVE)
int sustainedMotionLevel =
  0;

// 단계가 마지막으로 바뀐 시각 ('위험→주의→안정'이 연달아 바뀔 때 마지막 값만 보내기 위해)
unsigned long sustainedLevelChangedAt =
  0;


// 가장 최근 기록부터 거슬러 올라가며, 연속으로 threshold 이상이었던 초 수
int consecutiveSecondsAbove(
  int threshold
) {

  int count =
    0;

  for (
    int i = 0;
    i < motionSecCount;
    i++
  ) {

    int idx =
      (motionSecHead - 1 - i + MOTION_HISTORY_SEC) % MOTION_HISTORY_SEC;

    if (
      motionSecMax[idx] < threshold
    ) {

      break;
    }

    count++;
  }

  return count;
}


// 지금 진행 중인 1초 + 바로 전 1초 동안의 최대 움직임
int recentMaxMotion() {

  int result =
    max(motionBinMax, 0);

  if (
    motionSecCount > 0
  ) {

    int idx =
      (motionSecHead - 1 + MOTION_HISTORY_SEC) % MOTION_HISTORY_SEC;

    result =
      max(result, motionSecMax[idx]);
  }

  return result;
}


// 가만히 있었으면 지금까지 쌓인 '계속 움직임' 기록을 지움 (다시 처음부터 쌓아야 올라감)
void clearMotionHistory(
  unsigned long now
) {

  motionSecCount =
    0;

  motionSecHead =
    0;

  motionBinStart =
    now;

  motionBinMax =
    -1;
}


// 큰 움직임(60점↑)이 끊겼으면 '위험' 쪽 기록만 지움 ('주의' 쪽 기록은 유지)
void capMotionHistory(
  int cap
) {

  for (
    int i = 0;
    i < MOTION_HISTORY_SEC;
    i++
  ) {

    if (
      motionSecMax[i] > cap
    ) {

      motionSecMax[i] =
        cap;
    }
  }

  if (
    motionBinMax > cap
  ) {

    motionBinMax =
      cap;
  }
}


// readMovement()에서 움직임 점수를 계산할 때마다 호출
void updateSustainedMotion(
  int score
) {

  unsigned long now =
    millis();

  int levelBefore =
    sustainedMotionLevel;

  if (
    lastMotionUpdateAt == 0
  ) {

    lastMotionUpdateAt =
      now;

    lastAboveCalmAt =
      now;

    lastAboveDangerAt =
      now;

    motionBinStart =
      now;
  }


  // Wi-Fi 전송이나 음성 AI로 측정이 멈췄던 시간은 '가만히 있던 시간'으로 치지 않음
  // (흔드는 도중 전송 때문에 단계가 잘못 내려가지 않도록, 멈춘 시간만큼 시계를 밀어줌)
  unsigned long gap =
    now - lastMotionUpdateAt;

  if (
    gap > 300
  ) {

    unsigned long paused =
      gap - 20;

    lastAboveCalmAt +=
      paused;

    lastAboveDangerAt +=
      paused;

    motionBinStart +=
      paused;
  }

  lastMotionUpdateAt =
    now;


  if (
    score >= CALM_MOTION_SCORE
  ) {

    lastAboveCalmAt =
      now;
  }

  if (
    score >= 60
  ) {

    lastAboveDangerAt =
      now;
  }


  // 1초가 지나면 그 1초의 최대값을 기록
  if (
    now - motionBinStart >= 1000
  ) {

    if (
      motionBinMax >= 0
    ) {

      motionSecMax[motionSecHead] =
        motionBinMax;

      motionSecHead =
        (motionSecHead + 1) % MOTION_HISTORY_SEC;

      if (
        motionSecCount < MOTION_HISTORY_SEC
      ) {

        motionSecCount++;
      }
    }

    motionBinMax =
      -1;

    motionBinStart =
      now;
  }


  if (
    score > motionBinMax
  ) {

    motionBinMax =
      score;
  }


  // ---------- 내려가기 (빠르게) ----------

  // 60점 이상 큰 움직임이 CALM_HOLD_MS 동안 없으면: '위험' → '주의'
  if (
    now - lastAboveDangerAt >= CALM_HOLD_MS
  ) {

    capMotionHistory(59);

    if (
      sustainedMotionLevel == 2
    ) {

      sustainedMotionLevel =
        1;
    }
  }

  // 가만히(CALM_MOTION_SCORE 미만) CALM_HOLD_MS 동안 있으면: 바로 '안정'
  if (
    now - lastAboveCalmAt >= CALM_HOLD_MS
  ) {

    clearMotionHistory(now);

    motionBinMax =
      score;

    sustainedMotionLevel =
      0;
  }


  // ---------- 올라가기 (천천히) ----------

  int upLevel =
    0;

  if (
    consecutiveSecondsAbove(60) >= DANGER_HOLD_SEC
  ) {

    upLevel =
      2;

  } else if (
    consecutiveSecondsAbove(20) >= CAUTION_HOLD_SEC
  ) {

    upLevel =
      1;
  }

  if (
    upLevel > sustainedMotionLevel
  ) {

    sustainedMotionLevel =
      upLevel;
  }


  if (
    sustainedMotionLevel != levelBefore
  ) {

    sustainedLevelChangedAt =
      now;
  }
}


// 웹사이트로 보낼 움직임 점수.
// 웹사이트는 점수로도 단계를 판단하므로(12점↑ 주의, 20점↑ 주의, 60점↑ 위험),
// 판정된 단계의 범위 안으로 점수를 맞춰서 보냄
int sustainedMotionScore() {

  int recent =
    recentMaxMotion();

  if (
    sustainedMotionLevel == 2
  ) {

    return constrain(recent, 60, 100);
  }

  if (
    sustainedMotionLevel == 1
  ) {

    return constrain(recent, 20, 59);
  }

  return constrain(recent, 0, 11);
}


const char* motionLevelFor(
  int score
) {

  if (
    score < 20
  ) {

    return "NORMAL";
  }

  if (
    score < 60
  ) {

    return "BIG MOVE";
  }

  return "BIG BIG MOVE";
}


// ============================================================
// Microphone
// ============================================================

I2SClass I2S;


// Edge Impulse 모델의 실제 샘플링 주파수 사용
#define AUDIO_SAMPLE_RATE EI_CLASSIFIER_FREQUENCY

#define AUDIO_SAMPLE_COUNT \
  EI_CLASSIFIER_RAW_SAMPLE_COUNT


static int16_t audioBuffer[
  AUDIO_SAMPLE_COUNT
];

static float audioFloatBuffer[
  AUDIO_SAMPLE_COUNT
];

bool micReady =
  false;


// ============================================================
// Voice AI
// ============================================================

String voiceLabel =
  "unknown";

float voiceConfidence =
  0.0;

unsigned long lastVoiceAI =
  0;


// 음성 AI 실행 간격
// 10초마다 한 번
const unsigned long VOICE_AI_INTERVAL =
  10000;


// ============================================================
// 시간
// ============================================================

unsigned long lastTelemetry =
  0;

// 상태가 바뀌면 10초를 기다리지 않고 바로 전송 (단, 최소 3초 간격)
const unsigned long TELEMETRY_INTERVAL_MS =
  10000;

const unsigned long TELEMETRY_MIN_GAP_MS =
  3000;

// [v4] 단계가 '내려갈 때'는 3초를 기다리지 않고 1초 간격이면 바로 전송
const unsigned long TELEMETRY_DROP_GAP_MS =
  1000;

// [v4] '위험→주의→안정'처럼 연달아 내려갈 때 중간값을 따로 보내지 않도록 0.2초만 기다림
const unsigned long TELEMETRY_DROP_SETTLE_MS =
  200;

int lastSentMotionLevel =
  0;   // 0: NORMAL, 1: BIG MOVE, 2: BIG BIG MOVE

bool lastSentContact =
  false;


int motionLevelIndex(
  int score
) {

  if (
    score < 20
  ) {

    return 0;
  }

  if (
    score < 60
  ) {

    return 1;
  }

  return 2;
}

unsigned long lastMotionRead =
  0;

unsigned long lastWiFiCheck =
  0;

unsigned long lastStatus =
  0;


// ============================================================
// 함수 선언
// ============================================================

void connectWiFi();

void setupMAX30102();

void calibrateMAX30102();

void readHeartRate();

void setupGY61();

void calibrateGY61();

void readMovement();

bool initMicrophone();

bool recordAudio();

bool runVoiceAI();

bool postTelemetryWithRedirect(
  const String& payload,
  String currentUrl
);

void sendTelemetry();

void printStatus();


// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);

  // USB로 컴퓨터에 연결돼 있지만 시리얼 모니터가 닫혀 있을 때
  // 출력이 쌓여 보드가 멈추지 않도록, 출력 대기 시간을 0으로 설정
  Serial.setTxTimeoutMs(0);

  delay(2000);

  Serial.println();
  Serial.println();
  Serial.println(
    "================================="
  );

  Serial.println(
    "      CALM FIT FINAL SYSTEM (v4 빠른진정)"
  );

  Serial.println(
    "================================="
  );

  Serial.println();


  // ========================================================
  // I2C
  // ========================================================

  Wire.begin(
    D4,
    D5
  );


  // ========================================================
  // MAX30102
  // ========================================================

  setupMAX30102();


  // ========================================================
  // GY-61
  // ========================================================

  setupGY61();


  // ========================================================
  // Microphone
  // ========================================================

  Serial.println();
  Serial.println(
    "[3] XIAO Sense 마이크 초기화"
  );

  micReady =
    initMicrophone();


  if (micReady) {

    Serial.println(
      "✅ 마이크 초기화 성공!"
    );

  } else {

    Serial.println(
      "❌ 마이크 초기화 실패"
    );
  }


  // ========================================================
  // Edge Impulse
  // ========================================================

  Serial.println();
  Serial.println(
    "[4] Edge Impulse"
  );

  Serial.print(
    "샘플 수: "
  );

  Serial.println(
    EI_CLASSIFIER_RAW_SAMPLE_COUNT
  );

  Serial.print(
    "샘플링 주파수: "
  );

  Serial.println(
    EI_CLASSIFIER_FREQUENCY
  );

  Serial.print(
    "클래스 수: "
  );

  Serial.println(
    EI_CLASSIFIER_LABEL_COUNT
  );


  // ========================================================
  // Wi-Fi
  // ========================================================

  Serial.println();
  Serial.println(
    "[5] Wi-Fi 연결"
  );

  WiFi.mode(
    WIFI_STA
  );

  for (
    const WifiNetwork& network : WIFI_NETWORKS
  ) {

    wifiMulti.addAP(
      network.ssid,
      network.password
    );
  }

  connectWiFi();


  // ========================================================
  // 완료
  // ========================================================

  Serial.println();
  Serial.println(
    "================================="
  );

  Serial.println(
    "       CALM FIT 준비 완료"
  );

  Serial.println(
    "================================="
  );

  Serial.println();

}


// ============================================================
// LOOP
// ============================================================

void loop() {

  // ========================================================
  // MAX30102
  // ========================================================

  readHeartRate();


  // ========================================================
  // GY-61
  // ========================================================

  if (
    millis() - lastMotionRead >= 20
  ) {

    lastMotionRead =
      millis();

    readMovement();
  }


  // ========================================================
  // Wi-Fi 확인
  // ========================================================

  if (
    // Wi-Fi가 끊겼을 때 재연결 시도는 30초마다 (재연결 중에는 센서 측정이 멈추기 때문)
    millis() - lastWiFiCheck >= 30000
  ) {

    lastWiFiCheck =
      millis();

    if (
      WiFi.status() != WL_CONNECTED
    ) {

      Serial.println(
        "[WiFi] 연결 끊김 → 재연결"
      );

      connectWiFi();
    }
  }


  // ========================================================
  // 음성 AI
  // ========================================================

  if (
    micReady &&
    millis() - lastVoiceAI >= VOICE_AI_INTERVAL
  ) {

    lastVoiceAI =
      millis();

    if (recordAudio()) {

      runVoiceAI();
    }
  }


  // ========================================================
  // 서버 전송
  // ========================================================

  // [v4] 순간 움직임이 아니라 '판정된' 단계를 기준으로 전송
  // (올라갈 때 N초 지속, 내려갈 때 CALM_HOLD_MS 가만히 — 판정 안에 들어 있음)
  int currentMotionLevel =
    sustainedMotionLevel;

  bool sinceLastSend =
    millis() - lastTelemetry >= TELEMETRY_MIN_GAP_MS;

  bool sinceLastSendForDrop =
    millis() - lastTelemetry >= TELEMETRY_DROP_GAP_MS;

  // 1) 단계가 올라가면 바로(최소 3초 간격)  2) 단계가 내려가면 바로(최소 1초 간격)
  // 3) 손가락 접촉이 바뀌면 바로  4) 그 외에는 10초마다
  bool levelRose =
    currentMotionLevel > lastSentMotionLevel;

  bool levelDropped =
    currentMotionLevel < lastSentMotionLevel &&
    millis() - sustainedLevelChangedAt >= TELEMETRY_DROP_SETTLE_MS;

  bool contactChanged =
    fingerDetected != lastSentContact;

  bool sendNow =
    millis() - lastTelemetry >= TELEMETRY_INTERVAL_MS ||
    (sinceLastSend && (levelRose || contactChanged)) ||
    (sinceLastSendForDrop && levelDropped);


  if (
    sendNow
  ) {

    lastTelemetry =
      millis();

    lastSentMotionLevel =
      currentMotionLevel;

    lastSentContact =
      fingerDetected;

    sendTelemetry();
  }


  // ========================================================
  // 상태 출력
  // ========================================================

  printStatus();

  delay(1);
}


// ============================================================
// MAX30102 초기화
// ============================================================

void setupMAX30102() {

  Serial.println();
  Serial.println(
    "[1] MAX30102 초기화"
  );


  if (
    !particleSensor.begin(
      Wire,
      I2C_SPEED_FAST
    )
  ) {

    Serial.println(
      "❌ MAX30102를 찾을 수 없습니다."
    );

    heartSensorReady =
      false;

    return;
  }


  Serial.println(
    "✅ MAX30102 연결 성공!"
  );


  heartSensorReady =
    true;


  particleSensor.setup(
    0x10,
    4,
    2,
    100,
    411,
    4096
  );


  particleSensor.setPulseAmplitudeRed(
    0x0A
  );

  particleSensor.setPulseAmplitudeIR(
    0x10
  );

  particleSensor.setPulseAmplitudeGreen(
    0
  );


  Serial.println(
    "✅ MAX30102 설정 완료"
  );


  calibrateMAX30102();
}


// ============================================================
// MAX30102 보정
// ============================================================

void calibrateMAX30102() {

  Serial.println();
  Serial.println(
    "[MAX30102] 손가락을 센서에서 떼세요."
  );

  Serial.println(
    "[MAX30102] 약 2초간 기준값 측정..."
  );

  delay(2000);


  long total =
    0;

  int count =
    0;


  for (
    int i = 0;
    i < 60;
    i++
  ) {

    long value =
      particleSensor.getIR();


    if (value > 0) {

      total +=
        value;

      count++;
    }

    delay(30);
  }


  if (count > 0) {

    irNoFingerBaseline =
      total / count;

  } else {

    irNoFingerBaseline =
      1000;
  }


  fingerOnThreshold =
    irNoFingerBaseline + 8000;


  if (
    fingerOnThreshold < 10000
  ) {

    fingerOnThreshold =
      10000;
  }


  // 켤 때 손가락이 센서에 올라가 있어 기준이 너무 높게 잡혀도
  // 손가락(IR 약 5~6만)을 인식할 수 있도록 상한을 3만으로 제한
  if (
    fingerOnThreshold > 30000
  ) {

    fingerOnThreshold =
      30000;
  }


  Serial.println();
  Serial.println(
    "✅ MAX30102 보정 완료"
  );

  Serial.print(
    "손가락 없음 기준 IR = "
  );

  Serial.println(
    irNoFingerBaseline
  );

  Serial.print(
    "손가락 감지 기준 IR = "
  );

  Serial.println(
    fingerOnThreshold
  );

  Serial.println();
}


// ============================================================
// MAX30102 심박수
// ============================================================

void readHeartRate() {

  if (!heartSensorReady) {

    bpm =
      0;

    fingerDetected =
      false;

    return;
  }


  irValue =
    particleSensor.getIR();


  // 손가락 없음
  if (
    irValue < fingerOnThreshold
  ) {

    fingerDetected =
      false;

    bpm =
      0;

    lastBeat =
      0;

    bpmHistoryCount =
      0;

    return;
  }


  fingerDetected =
    true;


  if (
    bpm > 0 &&
    millis() - lastValidBeatAt > BPM_EXPIRE_MS
  ) {

    bpm =
      0;

    bpmHistoryCount =
      0;
  }


  if (
    checkForBeat(irValue)
  ) {

    unsigned long currentTime =
      millis();


    if (
      lastBeat > 0
    ) {

      long delta =
        currentTime -
        lastBeat;


      if (
        delta > 250 &&
        delta < 1500
      ) {

        float newBPM =
          60000.0 /
          delta;


        if (
          newBPM >= 40 &&
          newBPM <= 200
        ) {

          bpmHistory[
            bpmHistoryCount % BPM_AVERAGE_COUNT
          ] = newBPM;

          bpmHistoryCount++;

          int averageCount =
            min(bpmHistoryCount, BPM_AVERAGE_COUNT);

          float sum =
            0;

          for (
            int i = 0;
            i < averageCount;
            i++
          ) {

            sum +=
              bpmHistory[i];
          }

          bpm =
            sum / averageCount;

          lastValidBeatAt =
            millis();

          Serial.print(
            "❤️ BPM: "
          );

          Serial.println(
            bpm,
            1
          );
        }
      }
    }


    lastBeat =
      currentTime;
  }


  if (
    millis() - lastHeartPrint >= 500
  ) {

    lastHeartPrint =
      millis();

    Serial.print(
      "[MAX30102] IR="
    );

    Serial.print(
      irValue
    );

    Serial.print(
      " / 기준="
    );

    Serial.print(
      fingerOnThreshold
    );

    Serial.print(
      " / Contact="
    );

    Serial.print(
      fingerDetected
        ? "YES"
        : "NO"
    );

    Serial.print(
      " / BPM="
    );

    Serial.println(
      bpm
    );
  }
}


// ============================================================
// GY-61 초기화
// ============================================================

void setupGY61() {

  Serial.println();
  Serial.println(
    "[2] GY-61 초기화"
  );


  pinMode(
    GY61_X,
    INPUT
  );

  pinMode(
    GY61_Y,
    INPUT
  );

  pinMode(
    GY61_Z,
    INPUT
  );


  calibrateGY61();
}


// ============================================================
// GY-61 보정
// ============================================================

// 아날로그 잡음을 줄이기 위해 같은 축을 여러 번 읽어 평균
const int GY61_OVERSAMPLE =
  8;

int readAxisAveraged(
  int pin
) {

  long sum =
    0;

  for (
    int i = 0;
    i < GY61_OVERSAMPLE;
    i++
  ) {

    sum +=
      analogRead(pin);
  }

  return sum / GY61_OVERSAMPLE;
}


void calibrateGY61() {

  Serial.println();
  Serial.println(
    "================================="
  );

  Serial.println(
    "GY-61 움직임 기준값 설정"
  );

  Serial.println(
    "================================="
  );

  Serial.println(
    "⚠️ 3초 동안 손목을 가만히 두세요."
  );


  delay(3000);


  long sumX =
    0;

  long sumY =
    0;

  long sumZ =
    0;

  long sumNoise =
    0;


  int lastCalX =
    readAxisAveraged(GY61_X);

  int lastCalY =
    readAxisAveraged(GY61_Y);

  int lastCalZ =
    readAxisAveraged(GY61_Z);


  for (
    int i = 0;
    i < 100;
    i++
  ) {

    int x =
      readAxisAveraged(GY61_X);

    int y =
      readAxisAveraged(GY61_Y);

    int z =
      readAxisAveraged(GY61_Z);


    sumX +=
      x;

    sumY +=
      y;

    sumZ +=
      z;


    sumNoise +=
      abs(x - lastCalX) +
      abs(y - lastCalY) +
      abs(z - lastCalZ);


    lastCalX =
      x;

    lastCalY =
      y;

    lastCalZ =
      z;


    delay(10);
  }


  baseX =
    sumX / 100;

  baseY =
    sumY / 100;

  baseZ =
    sumZ / 100;


  prevX =
    baseX;

  prevY =
    baseY;

  prevZ =
    baseZ;


  // 가만히 있을 때의 평균 흔들림의 2배(+2)까지는 움직임이 아닌 잡음으로 봅니다.
  noiseFloor =
    (sumNoise / 100) * 2 + 2;


  if (
    noiseFloor < 3
  ) {

    noiseFloor =
      3;
  }


  if (
    noiseFloor > 60
  ) {

    noiseFloor =
      60;
  }


  Serial.println();
  Serial.println(
    "✅ GY-61 기준값 설정 완료"
  );

  Serial.print(
    "기준 X = "
  );

  Serial.println(
    baseX
  );

  Serial.print(
    "기준 Y = "
  );

  Serial.println(
    baseY
  );

  Serial.print(
    "기준 Z = "
  );

  Serial.println(
    baseZ
  );

  Serial.print(
    "잡음 기준값 = "
  );

  Serial.println(
    noiseFloor
  );
}


// ============================================================
// GY-61 움직임
// ============================================================

void readMovement() {

  int x =
    readAxisAveraged(GY61_X);

  int y =
    readAxisAveraged(GY61_Y);

  int z =
    readAxisAveraged(GY61_Z);


  int dx =
    abs(x - prevX);

  int dy =
    abs(y - prevY);

  int dz =
    abs(z - prevZ);


  rawMotion =
    dx +
    dy +
    dz;


  int effectiveMotion =
    rawMotion -
    noiseFloor;


  if (
    effectiveMotion < 0
  ) {

    effectiveMotion =
      0;
  }


  // 기존보다 움직임에 민감하게
  int newScore =
    map(
      effectiveMotion,
      0,
      120,
      0,
      100
    );


  newScore =
    constrain(
      newScore,
      0,
      100
    );


  static int previousScore =
    0;


  // 올라갈 때는 기존처럼 부드럽게, 진정될 때는 3배 빠르게 내려가도록 비대칭 평활
  if (
    newScore >= previousScore
  ) {

    motionScore =
      (
        previousScore * 3 +
        newScore
      ) / 4;

  } else {

    motionScore =
      (
        previousScore +
        newScore * 3
      ) / 4;
  }


  previousScore =
    motionScore;


  if (
    motionScore < 20
  ) {

    motionLevel =
      "NORMAL";

  } else if (
    motionScore < 60
  ) {

    motionLevel =
      "BIG MOVE";

  } else {

    motionLevel =
      "BIG BIG MOVE";
  }


  // [v3] 1초 단위 기록 + 지속 단계 판정
  updateSustainedMotion(
    motionScore
  );


  prevX =
    x;

  prevY =
    y;

  prevZ =
    z;
}


// ============================================================
// 마이크 초기화
// ============================================================

bool initMicrophone() {

  I2S.setPinsPdmRx(
    42,
    41
  );


  bool result =
    I2S.begin(
      I2S_MODE_PDM_RX,
      AUDIO_SAMPLE_RATE,
      I2S_DATA_BIT_WIDTH_16BIT,
      I2S_SLOT_MODE_MONO
    );


  if (!result) {

    return false;
  }


  delay(500);

  return true;
}


// ============================================================
// 음성 녹음
// ============================================================

bool recordAudio() {

  if (!micReady) {

    return false;
  }


  Serial.println();
  Serial.println(
    "🎤 음성 데이터 수집 중..."
  );


  for (
    size_t i = 0;
    i < AUDIO_SAMPLE_COUNT;
    i++
  ) {

    int sample =
      I2S.read();


    if (
      sample == -1
    ) {

      audioBuffer[i] =
        0;

    } else {

      audioBuffer[i] =
        (int16_t)sample;
    }


    // 녹음 중에도 센서 업데이트
    if (
      i % 1000 == 0
    ) {

      readHeartRate();
      readMovement();
    }
  }


  Serial.println(
    "🎤 음성 데이터 수집 완료"
  );


  return true;
}


// ============================================================
// Edge Impulse 음성 AI
// ============================================================

bool runVoiceAI() {

  for (
    size_t i = 0;
    i < AUDIO_SAMPLE_COUNT;
    i++
  ) {

    audioFloatBuffer[i] =
      (float)audioBuffer[i];
  }


  signal_t signal;


  int signalError =
    numpy::signal_from_buffer(
      audioFloatBuffer,
      AUDIO_SAMPLE_COUNT,
      &signal
    );


  if (
    signalError != 0
  ) {

    Serial.print(
      "❌ signal 생성 실패: "
    );

    Serial.println(
      signalError
    );

    return false;
  }


  ei_impulse_result_t result =
    { 0 };


  EI_IMPULSE_ERROR res =
    run_classifier(
      &signal,
      &result,
      false
    );


  if (
    res != EI_IMPULSE_OK
  ) {

    Serial.print(
      "❌ 음성 AI 오류: "
    );

    Serial.println(
      res
    );

    return false;
  }


  int bestIndex =
    0;


  float bestValue =
    result.classification[0].value;


  for (
    size_t i = 1;
    i < EI_CLASSIFIER_LABEL_COUNT;
    i++
  ) {

    if (
      result.classification[i].value >
      bestValue
    ) {

      bestValue =
        result.classification[i].value;

      bestIndex =
        i;
    }
  }


  voiceLabel =
    result.classification[
      bestIndex
    ].label;


  voiceConfidence =
    result.classification[
      bestIndex
    ].value;


  Serial.println();
  Serial.println(
    "===== 음성 AI 결과 ====="
  );


  for (
    size_t i = 0;
    i < EI_CLASSIFIER_LABEL_COUNT;
    i++
  ) {

    Serial.print(
      result.classification[i].label
    );

    Serial.print(
      ": "
    );

    Serial.print(
      result.classification[i].value * 100.0,
      1
    );

    Serial.println(
      "%"
    );
  }


  Serial.print(
    "▶ 최종 인식: "
  );

  Serial.print(
    voiceLabel
  );

  Serial.print(
    " ("
  );

  Serial.print(
    voiceConfidence * 100.0,
    1
  );

  Serial.println(
    "%)"
  );


  return true;
}


// ============================================================
// Wi-Fi
// ============================================================

void connectWiFi() {

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    return;
  }


  Serial.println();
  Serial.println(
    "Wi-Fi 연결 중: 등록된 Wi-Fi 중 신호가 잡히는 곳을 찾는 중..."
  );


  // 근처 Wi-Fi를 검색해서 등록된 것 중 신호가 가장 강한 곳에 연결 (최대 8초)
  if (
    wifiMulti.run(8000) ==
    WL_CONNECTED
  ) {

    Serial.print(
      "✅ Wi-Fi 연결 성공: "
    );

    Serial.println(
      WiFi.SSID()
    );

    Serial.print(
      "ESP32 IP: "
    );

    Serial.println(
      WiFi.localIP()
    );

  } else {

    Serial.println(
      "❌ Wi-Fi 연결 실패: 등록된 Wi-Fi가 근처에 없거나 비밀번호가 다릅니다."
    );


    // 원인 확인용: 보드가 볼 수 있는 주변 Wi-Fi 이름 출력 (대소문자까지 똑같아야 연결됨)
    int found =
      WiFi.scanNetworks();

    Serial.printf(
      "📡 주변 Wi-Fi %d개:\n",
      found
    );

    for (
      int i = 0;
      i < found;
      i++
    ) {

      Serial.printf(
        "   [%s] 신호 %d dBm, 채널 %d\n",
        WiFi.SSID(i).c_str(),
        WiFi.RSSI(i),
        WiFi.channel(i)
      );
    }

    WiFi.scanDelete();
  }
}


// ============================================================
// HTTPS POST + REDIRECT
// ============================================================

bool postTelemetryWithRedirect(
  const String& payload,
  String currentUrl
) {

  const int MAX_REDIRECTS =
    3;


  for (
    int attempt = 0;
    attempt <= MAX_REDIRECTS;
    attempt++
  ) {

    Serial.println();
    Serial.println(
      "========================================"
    );

    Serial.printf(
      "CALM FIT POST %d/%d\n",
      attempt + 1,
      MAX_REDIRECTS + 1
    );

    Serial.println(
      "URL: " + currentUrl
    );


    WiFiClientSecure client;

    // 대회용
    // 서버 인증서 검증은 하지 않음
    client.setInsecure();


    HTTPClient http;


    http.setConnectTimeout(
      10000
    );

    http.setTimeout(
      15000
    );


    // ESP32가 redirect를 직접 처리
    http.setFollowRedirects(
      HTTPC_DISABLE_FOLLOW_REDIRECTS
    );


    const char* headers[] =
      {
        "Location"
      };


    http.collectHeaders(
      headers,
      1
    );


    if (
      !http.begin(
        client,
        currentUrl
      )
    ) {

      Serial.println(
        "❌ HTTP begin 실패"
      );

      return false;
    }


    // ======================================================
    // ⭐ 인증 헤더
    // ======================================================

    http.addHeader(
      "Content-Type",
      "application/json"
    );


    http.addHeader(
      "X-CALMFIT-DEVICE-TOKEN",
      DEVICE_TOKEN
    );


    // ⭐ 수정: 위의 DEVICE_TOKEN 값은 Vercel의 CALMFIT_ACCESS_TOKEN(보호자 토큰)과 같은 값이라
    // X-CALMFIT-DEVICE-TOKEN 헤더만으로는 401이 납니다.
    // Authorization: Bearer 헤더로도 함께 보내면 서버가 인증합니다. (둘 다 보내도 정상 동작 확인됨)
    http.addHeader(
      "Authorization",
      String("Bearer ") + DEVICE_TOKEN
    );

    Serial.println(
      "🔑 [v2] Authorization: Bearer 헤더 포함해서 전송"
    );


    http.addHeader(
      "X-CALMFIT-DEVICE-ID",
      DEVICE_ID
    );


    // ======================================================
    // POST
    // ======================================================

    int httpCode =
      http.POST(
        payload
      );


    String response =
      http.getString();


    String location =
      http.header(
        "Location"
      );


    Serial.printf(
      "HTTP Code: %d\n",
      httpCode
    );


    if (
      response.length() > 0
    ) {

      Serial.println(
        "Response:"
      );

      Serial.println(
        response
      );
    }


    // ======================================================
    // 성공
    // ======================================================

    if (
      httpCode >= 200 &&
      httpCode < 300
    ) {

      Serial.println(
        "✅ SUCCESS: Telemetry sent!"
      );

      http.end();

      return true;
    }


    // ======================================================
    // Redirect
    // ======================================================

    if (
      (
        httpCode == 301 ||
        httpCode == 302 ||
        httpCode == 303 ||
        httpCode == 307 ||
        httpCode == 308
      ) &&
      location.length() > 0
    ) {

      Serial.println(
        "↪️ REDIRECT detected"
      );

      Serial.println(
        "Location:"
      );

      Serial.println(
        location
      );


      // 절대 URL이면 그대로 사용
      if (
        location.startsWith(
          "http://"
        ) ||
        location.startsWith(
          "https://"
        )
      ) {

        currentUrl =
          location;
      }

      else {

        // 상대 경로 처리
        if (
          location.startsWith("/")
        ) {

          int schemeEnd =
            currentUrl.indexOf(
              "://"
            );

          int hostEnd =
            currentUrl.indexOf(
              "/",
              schemeEnd + 3
            );


          if (
            hostEnd > 0
          ) {

            currentUrl =
              currentUrl.substring(
                0,
                hostEnd
              ) +
              location;

          } else {

            currentUrl =
              currentUrl +
              location;
          }
        }
      }


      http.end();

      delay(200);

      continue;
    }


    // ======================================================
    // 401
    // ======================================================

    if (
      httpCode == 401
    ) {

      Serial.println(
        "❌ ERROR 401"
      );

      Serial.println(
        "서버가 기기 인증 토큰을 받지 못했거나"
      );

      Serial.println(
        "Vercel의 CALMFIT_DEVICE_TOKEN과"
      );

      Serial.println(
        "ESP32 DEVICE_TOKEN을 확인하세요."
      );

      http.end();

      return false;
    }


    // ======================================================
    // 403
    // ======================================================

    if (
      httpCode == 403
    ) {

      Serial.println(
        "❌ ERROR 403"
      );

      Serial.println(
        "서버 접근이 거부되었습니다."
      );

      http.end();

      return false;
    }


    // ======================================================
    // 503
    // ======================================================

    if (
      httpCode == 503
    ) {

      Serial.println(
        "❌ ERROR 503"
      );

      Serial.println(
        "Vercel 환경변수 설정을 확인하세요."
      );

      http.end();

      return false;
    }


    // ======================================================
    // 기타
    // ======================================================

    Serial.printf(
      "❌ HTTP ERROR: %d\n",
      httpCode
    );


    http.end();

    return false;
  }


  Serial.println(
    "❌ ERROR: Too many redirects."
  );


  return false;
}


// ============================================================
// 서버 전송
// ============================================================

void sendTelemetry() {

  if (
    WiFi.status() != WL_CONNECTED
  ) {

    Serial.println(
      "[Telemetry] Wi-Fi 없음"
    );

    return;
  }


  // ========================================================
  // JSON 생성
  // ========================================================

  String json =
    "{";


  json +=
    "\"deviceId\":\"";

  json +=
    DEVICE_ID;

  json +=
    "\",";



  json +=
    "\"heartRate\":";

  json +=
    String(
      bpm,
      1
    );

  json +=
    ",";


  json +=
    "\"rawIrValue\":";

  json +=
    String(
      irValue
    );

  json +=
    ",";


  json +=
    "\"contact\":";

  json +=
    (
      fingerDetected
        ? "true"
        : "false"
    );

  json +=
    ",";


  json +=
    "\"motionScore\":";

  // [v3] 순간 최고값 대신 '지속 판정된' 점수/단계를 보냄
  int sendMotionScore =
    sustainedMotionScore();

  json +=
    String(
      sendMotionScore
    );

  json +=
    ",";


  json +=
    "\"motionLevel\":\"";

  json +=
    motionLevelFor(
      sendMotionScore
    );

  json +=
    "\",";


  json +=
    "\"accelX\":";

  json +=
    String(
      prevX
    );

  json +=
    ",";


  json +=
    "\"accelY\":";

  json +=
    String(
      prevY
    );

  json +=
    ",";


  json +=
    "\"accelZ\":";

  json +=
    String(
      prevZ
    );

  json +=
    ",";


  json +=
    "\"voiceLabel\":\"";

  json +=
    voiceLabel;

  json +=
    "\",";


  json +=
    "\"voiceConfidence\":";

  json +=
    String(
      voiceConfidence,
      4
    );


  json +=
    "}";


  // ========================================================
  // 서버로 전송
  // ========================================================

  Serial.println();
  Serial.println(
    "========== CALM FIT TELEMETRY =========="
  );

  Serial.println(
    json
  );


  bool success =
    postTelemetryWithRedirect(
      json,
      SERVER_URL
    );


  Serial.println(
    "========================================"
  );


  if (success) {

    Serial.println(
      "✅ CALM FIT → SERVER : OK"
    );

  } else {

    Serial.println(
      "❌ CALM FIT → SERVER : FAILED"
    );
  }
}


// ============================================================
// 상태 출력
// ============================================================

void printStatus() {

  if (
    millis() - lastStatus < 2000
  ) {

    return;
  }


  lastStatus =
    millis();


  Serial.println();
  Serial.println(
    "------------- CALM FIT -------------"
  );


  Serial.print(
    "Heart Rate : "
  );

  Serial.println(
    bpm
  );


  Serial.print(
    "IR         : "
  );

  Serial.println(
    irValue
  );


  Serial.print(
    "Contact    : "
  );

  Serial.println(
    fingerDetected
      ? "YES"
      : "NO"
  );


  Serial.print(
    "Accel X    : "
  );

  Serial.println(
    prevX
  );


  Serial.print(
    "Accel Y    : "
  );

  Serial.println(
    prevY
  );


  Serial.print(
    "Accel Z    : "
  );

  Serial.println(
    prevZ
  );


  Serial.print(
    "Motion     : "
  );

  Serial.println(
    motionScore
  );


  Serial.print(
    "Level      : "
  );

  Serial.println(
    motionLevel
  );


  // [v3] 웹사이트로 보내는 지속 판정 단계 + 몇 초째 계속 움직이는 중인지
  Serial.print(
    "웹 단계용   : "
  );

  Serial.print(
    motionLevelFor(
      sustainedMotionScore()
    )
  );

  Serial.printf(
    " (점수 %d) | 20점↑ 연속 %d/%d초 | 60점↑ 연속 %d/%d초 | 가만히 %lums\n",
    sustainedMotionScore(),
    consecutiveSecondsAbove(20),
    CAUTION_HOLD_SEC,
    consecutiveSecondsAbove(60),
    DANGER_HOLD_SEC,
    millis() - lastAboveCalmAt
  );


  Serial.print(
    "Voice      : "
  );

  Serial.println(
    voiceLabel
  );


  Serial.print(
    "Confidence : "
  );

  Serial.print(
    voiceConfidence * 100.0,
    1
  );

  Serial.println(
    "%"
  );


  Serial.print(
    "WiFi       : "
  );


  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    Serial.println(
      "CONNECTED"
    );

  } else {

    Serial.println(
      "DISCONNECTED"
    );
  }


  Serial.println(
    "------------------------------------"
  );
}