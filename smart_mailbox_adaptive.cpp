/*
 * =====================================================
 *  УМНЫЙ ПОЧТОВЫЙ ЯЩИК - РЕЖИМ "АДАПТИВНЫЙ ПОРОГ"
 *  Версия: 5.1
 *  Описание: Измеряет расстояние до дна, вычисляет изменения.
 *            Порог срабатывания подстраивается под текущее состояние.
 *            Позволяет обнаруживать несколько писем последовательно.
 * =====================================================
 */

#include <SoftwareSerial.h>

// Создаём программный последовательный порт для связи с ESP-01S
SoftwareSerial esp(2, 3);

// ========== НАСТРОЙКИ ==========
String WIFI_NAME = "TP-Link_D28C";      // Имя Wi-Fi сети
String WIFI_PASS = "password";          // Пароль от Wi-Fi (ЗАМЕНИТЕ НА РЕАЛЬНЫЙ)
String PC_IP = "192.168.0.103";         // IP-адрес ПК с сервером

// ========== ПИНЫ ДАТЧИКА И СВЕТОДИОДА ==========
#define TRIG_PIN 9
#define ECHO_PIN 10
#define LED_PIN 13

// ========== ОПТИМИЗИРОВАННЫЕ ПАРАМЕТРЫ ==========
#define MAX_DISTANCE_CM 30           // Максимальная глубина ящика (см)
#define MIN_DISTANCE_CM 0.1          // Минимальное расстояние (см)
#define HYSTERESIS_CM 0.2            // Гистерезис для защиты от шумов
#define SAMPLE_COUNT 5               // Количество измерений для медианного фильтра
#define SAMPLE_DELAY_MS 50           // Задержка между измерениями в серии
#define LOOP_DELAY_MS 10000          // Пауза между основными циклами (10 секунд)
#define COOLDOWN_MS 5000             // Защита от повторных срабатываний (5 сек)
#define EMPTY_TOLERANCE_CM 0.3       // Допуск для определения пустого ящика

// ========== ПОРОГИ ОБНАРУЖЕНИЯ ==========
#define MAIL_DETECT_THRESHOLD_CM 0.15   // Абсолютное изменение для срабатывания (1.5 мм)
#define MAIL_DETECT_PERCENT 1.0        // Процентное изменение для срабатывания (1%)
#define ADDITIONAL_THRESHOLD_CM 0.11   // Порог для дополнительной почты (1.1 мм)

// ========== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ==========
float baselineDistance = 20.0;       // Базовое расстояние до дна (после калибровки)
float dynamicThreshold = 20.0;       // Текущий динамический порог (обновляется при срабатывании)
float lastStableDistance = 20.0;     // Последнее стабильное значение расстояния
unsigned long lastTriggerTime = 0;   // Время последнего срабатывания
bool alertSent = false;              // Флаг отправки уведомления
bool isEmpty = true;                 // Флаг: пуст ли ящик

// =====================================================
//  ФУНКЦИЯ: МЕДИАННЫЙ ФИЛЬТР
//  Принимает массив значений и возвращает медиану
//  Используется для отсечения выбросов
// =====================================================
float medianFilter(float* arr, int n) {
  float sorted[n];
  for (int i = 0; i < n; i++) sorted[i] = arr[i];
  
  // Пузырьковая сортировка для нахождения медианы
  for (int i = 0; i < n - 1; i++) {
    for (int j = i + 1; j < n; j++) {
      if (sorted[i] > sorted[j]) {
        float temp = sorted[i];
        sorted[i] = sorted[j];
        sorted[j] = temp;
      }
    }
  }
  return sorted[n / 2];   // Возвращаем средний элемент
}

// =====================================================
//  ФУНКЦИЯ: СТАБИЛЬНОЕ ИЗМЕРЕНИЕ РАССТОЯНИЯ
//  Делает SAMPLE_COUNT измерений, применяет медианный фильтр
//  и экспоненциальное сглаживание
// =====================================================
float getStableDistance() {
  float readings[SAMPLE_COUNT];
  
  for (int i = 0; i < SAMPLE_COUNT; i++) {
    // Генерация ультразвукового импульса
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    
    long duration = pulseIn(ECHO_PIN, HIGH, 60000);
    float distance = duration * 0.034 / 2;
    
    // Обработка ошибочных значений
    if (distance <= 0 || distance > MAX_DISTANCE_CM + 10) {
      distance = lastStableDistance;
    }
    if (distance < MIN_DISTANCE_CM) distance = MIN_DISTANCE_CM;
    if (distance > MAX_DISTANCE_CM) distance = MAX_DISTANCE_CM;
    
    readings[i] = distance;
    delay(SAMPLE_DELAY_MS);
  }
  
  // Применяем медианный фильтр
  float filtered = medianFilter(readings, SAMPLE_COUNT);
  
  // Экспоненциальное сглаживание (коэффициент 0.6 для предыдущего, 0.4 для нового)
  static float smoothed = 0;
  if (smoothed == 0) smoothed = filtered;
  smoothed = smoothed * 0.6 + filtered * 0.4;
  
  lastStableDistance = smoothed;
  return smoothed;
}

// =====================================================
//  ФУНКЦИЯ: ПРОВЕРКА ЗНАЧИМОСТИ ИЗМЕНЕНИЯ
//  Возвращает true, если изменение превышает абсолютный ИЛИ процентный порог
// =====================================================
bool hasSignificantChange(float current, float previous, float baseline) {
  float changeFromBaseline = previous - current;    // Насколько уменьшилось расстояние
  float percentChange = (changeFromBaseline / baseline) * 100;
  
  Serial.print("    Изменение: ");
  Serial.print(changeFromBaseline);
  Serial.print(" см (");
  Serial.print(percentChange);
  Serial.println("%)");
  
  // Проверка по абсолютному порогу
  bool byAbsolute = changeFromBaseline > MAIL_DETECT_THRESHOLD_CM;
  // Проверка по процентному порогу (плюс минимальное абсолютное изменение 0.1 см)
  bool byPercent = (percentChange > MAIL_DETECT_PERCENT) && (changeFromBaseline > 0.1);
  
  if (byAbsolute || byPercent) {
    Serial.print("    >>> СРАБОТАЛО: абсолютное=");
    Serial.print(byAbsolute);
    Serial.print(", процентное=");
    Serial.println(byPercent);
    return true;
  }
  return false;
}

// =====================================================
//  ФУНКЦИЯ: ИНТЕЛЛЕКТУАЛЬНОЕ ОБНАРУЖЕНИЕ ПОЧТЫ
//  Содержит всю логику работы адаптивного режима
// =====================================================
void smartDetection(float currentDist, float baseline, float &threshold) {
  static float history[10] = {0};
  static int historyIndex = 0;
  
  history[historyIndex] = currentDist;
  historyIndex = (historyIndex + 1) % 10;
  
  // === СЛУЧАЙ 1: ЯЩИК ОПУСТЕЛ ===
  // Если ранее была почта, а сейчас расстояние вернулось к базовому
  if (!isEmpty && abs(currentDist - baseline) < EMPTY_TOLERANCE_CM) {
    Serial.println("\n*** ЯЩИК ОПУСТЕЛ ***");
    threshold = baseline;          // Сбрасываем порог
    isEmpty = true;
    alertSent = false;
    digitalWrite(LED_PIN, LOW);
    sendToComputer("STATUS|Mailbox_empty|" + String(currentDist) + "_cm");
    return;
  }
  
  // === СЛУЧАЙ 2: ДОПОЛНИТЕЛЬНАЯ ПОЧТА ===
  // Если в ящике уже есть почта, отслеживаем дальнейшие уменьшения расстояния
  if (!isEmpty) {
    float additionalChange = threshold - currentDist;  // Изменение относительно предыдущего порога
    if (additionalChange > ADDITIONAL_THRESHOLD_CM && currentDist > 0) {
      if (millis() - lastTriggerTime > COOLDOWN_MS) {
        Serial.println("\n*** ОБНАРУЖЕНА ДОПОЛНИТЕЛЬНАЯ ПОЧТА ***");
        String msg = "MAIL|Additional|" + String(currentDist) + "_cm";
        sendToComputer(msg);
        threshold = currentDist;   // ОБНОВЛЯЕМ ПОРОГ! (ключевое отличие от быстрого режима)
        lastTriggerTime = millis();
        
        digitalWrite(LED_PIN, HIGH);
        delay(500);
        digitalWrite(LED_PIN, LOW);
      }
    }
  } 
  // === СЛУЧАЙ 3: ПЕРВАЯ ПОЧТА ===
  // Если ящик был пуст, проверяем изменение относительно базового расстояния
  else {
    if (hasSignificantChange(currentDist, baseline, baseline)) {
      if (millis() - lastTriggerTime > COOLDOWN_MS) {
        Serial.println("\n*** ПОЧТА ОБНАРУЖЕНА ***");
        digitalWrite(LED_PIN, HIGH);
        
        String msg = "MAIL|Detected|" + String(currentDist) + "_cm";
        sendToComputer(msg);
        
        threshold = currentDist;   // Устанавливаем новый порог
        lastTriggerTime = millis();
        alertSent = true;
        isEmpty = false;           // Ящик больше не пуст
        
        delay(2000);
        digitalWrite(LED_PIN, LOW);
      }
    }
  }
}

// =====================================================
//  ФУНКЦИЯ: ОТПРАВКА ДАННЫХ НА КОМПЬЮТЕР
//  Формирует HTTP GET запрос и отправляет через ESP-01S
// =====================================================
void sendToComputer(String data) {
  // Устанавливаем TCP соединение с сервером
  esp.println("AT+CIPSTART=\"TCP\",\"" + PC_IP + "\",8080");
  delay(500);
  
  // Формируем HTTP GET запрос
  String get = "GET /?data=" + data + " HTTP/1.1\r\n";
  get += "Host: " + PC_IP + "\r\n";
  get += "Connection: close\r\n\r\n";
  
  // Отправляем запрос через ESP
  esp.println("AT+CIPSEND=" + String(get.length()));
  delay(300);
  esp.print(get);
  delay(500);
  esp.println("AT+CIPCLOSE");
  
  Serial.print("Отправлено: ");
  Serial.println(data);
}

// =====================================================
//  ФУНКЦИЯ: КАЛИБРОВКА БАЗОВОГО РАССТОЯНИЯ
//  Делает 10 измерений, усредняет, получает расстояние до дна
// =====================================================
float calibrateBaseline() {
  Serial.println("\n=== КАЛИБРОВКА ===");
  Serial.println("Убедитесь, что почтовый ящик ПУСТ");
  Serial.println("Измерение расстояния до дна...");
  
  float readings[10];
  for (int i = 0; i < 10; i++) {
    readings[i] = getStableDistance();
    Serial.print(".");
    delay(500);
  }
  Serial.println();
  
  float baseline = medianFilter(readings, 10);
  
  Serial.print("Расстояние до дна: ");
  Serial.print(baseline);
  Serial.println(" см");
  
  return baseline;
}

// =====================================================
//  ФУНКЦИЯ: НАЧАЛЬНАЯ НАСТРОЙКА
// =====================================================
void setup() {
  Serial.begin(9600);
  esp.begin(115200);
  
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  
  Serial.println("\n========================================");
  Serial.println("     УМНЫЙ ПОЧТОВЫЙ ЯЩИК v5.1");
  Serial.println("     РЕЖИМ - АДАПТИВНЫЙ ПОРОГ");
  Serial.println("========================================");
  
  // Подключение к Wi-Fi
  Serial.println("[1] Подключение к Wi-Fi...");
  esp.println("AT+RST");
  delay(3000);
  esp.println("AT+CWMODE=1");
  delay(500);
  esp.println("AT+CWJAP=\"TP-Link_D28C\",\"65988237\"");
  delay(10000);
  
  Serial.println("[2] Wi-Fi подключён!");
  esp.println("AT+CIFSR");
  delay(1000);
  while (esp.available()) {
    Serial.write(esp.read());
  }
  
  // Калибровка
  baselineDistance = calibrateBaseline();
  dynamicThreshold = baselineDistance;
  isEmpty = true;
  
  Serial.println("\n[3] СИСТЕМА ГОТОВА!");
  Serial.print("[4] Порог обнаружения: ");
  Serial.print(MAIL_DETECT_THRESHOLD_CM);
  Serial.print(" см ИЛИ ");
  Serial.print(MAIL_DETECT_PERCENT);
  Serial.println("% изменения");
  Serial.println("========================================\n");
  
  sendToComputer("SYSTEM_START|Baseline_" + String(baselineDistance) + "_cm");
}

// =====================================================
//  ОСНОВНОЙ ЦИКЛ (ВЫПОЛНЯЕТСЯ БЕСКОНЕЧНО)
// =====================================================
void loop() {
  // Получаем стабильное значение расстояния
  float currentDist = getStableDistance();
  
  // Ограничиваем диапазон
  if (currentDist > MAX_DISTANCE_CM) {
    currentDist = MAX_DISTANCE_CM;
  }
  
  // Вывод информации в Serial Monitor
  Serial.print("[" + String(millis() / 1000) + "s] ");
  Serial.print("Расст: ");
  Serial.print(currentDist);
  Serial.print(" см | База: ");
  Serial.print(baselineDistance);
  Serial.print(" см | Разница: ");
  Serial.print(baselineDistance - currentDist);
  Serial.print(" см | Состояние: ");
  Serial.println(isEmpty ? "ПУСТ" : "ЕСТЬ ПОЧТА");
  
  // Запускаем алгоритм интеллектуального обнаружения
  smartDetection(currentDist, baselineDistance, dynamicThreshold);
  
  // Пауза 10 секунд до следующего цикла измерений
  delay(LOOP_DELAY_MS);
}