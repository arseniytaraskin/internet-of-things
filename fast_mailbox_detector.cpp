/*
 * =====================================================
 *  УМНЫЙ ПОЧТОВЫЙ ЯЩИК - РЕЖИМ БЫСТРОГО РЕАГИРОВАНИЯ
 *  Версия: 2.0
 *  Описание: Устройство измеряет расстояние каждые 50 мс.
 *            При обнаружении объекта ближе порога (6.2 см)
 *            отправляет уведомление и уходит в паузу на 7 сек.
 * =====================================================
 */

#include <SoftwareSerial.h>

// Создаём программный последовательный порт для связи с ESP-01S
// RX на пине 2 (приём данных от ESP), TX на пине 3 (передача данных в ESP)
SoftwareSerial esp(2, 3);

// ========== НАСТРОЙКИ Wi-Fi ==========
String WIFI_NAME = "TP-Link_D28C";      // Имя вашей Wi-Fi сети
String WIFI_PASS = "65988237";          // Пароль от Wi-Fi
String PC_IP = "192.168.0.103";         // IP-адрес ПК с Python-сервером

// ========== НАСТРОЙКИ ДАТЧИКА ==========
#define TRIG_PIN 9      // Пин для отправки ультразвукового импульса
#define ECHO_PIN 10     // Пин для приёма отражённого сигнала
#define LED_PIN 13      // Пин для светодиода (встроенный на Arduino UNO)

// ========== ПАРАМЕТРЫ АЛГОРИТМА ==========
#define MEASURE_DELAY_MS 50              // Задержка между измерениями (50 мс = 20 измерений/сек)
#define OBJECT_DETECT_THRESHOLD_CM 6.2   // Порог: расстояние МЕНЕЕ этого значения = есть письмо
#define COOLDOWN_MS 7000                 // Пауза после срабатывания (7 секунд)
#define DEBOUNCE_COUNT 2                 // Сколько измерений подряд нужно для подтверждения

// ========== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ==========
float baselineDistance = 0;          // Калибровочное расстояние до дна пустого ящика
unsigned long lastTriggerTime = 0;   // Время последнего срабатывания (для паузы)
bool mailPresent = false;            // Флаг: есть ли сейчас письмо в ящике
int confirmCounter = 0;              // Счётчик последовательных измерений для антидребезга

// =====================================================
//  ФУНКЦИЯ: ОДНОКРАТНОЕ ИЗМЕРЕНИЕ РАССТОЯНИЯ
//  Возвращает расстояние в сантиметрах (float)
// =====================================================
float measureDistance() {
  // Генерируем короткий импульс длительностью 10 мкс на пине TRIG
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  
  // Замеряем длительность импульса на пине ECHO (в микросекундах)
  // Таймаут 30 мс - если ответа нет, возвращаем 0
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  
  // Переводим время в расстояние: скорость звука 340 м/с = 0.034 см/мкс
  // Формула: расстояние = время * скорость звука / 2 (туда и обратно)
  float distance = duration * 0.034 / 2;
  
  // Обработка ошибочных значений
  if (distance <= 0 || distance > 50) {
    distance = 30;    // Если нет сигнала - считаем, что ящик пуст
  }
  if (distance < 0.5) {
    distance = 0.5;   // Минимальное расстояние - 0.5 см
  }
  
  return distance;
}

// =====================================================
//  ФУНКЦИЯ: ОТПРАВКА ДАННЫХ НА PYTHON-СЕРВЕР
//  Параметры: eventType - тип события, distance - расстояние
// =====================================================
void sendToServer(String eventType, float distance) {
  // Формируем строку данных, например: "MAIL|Detected|15.60_cm"
  String data = eventType + "|" + String(distance) + "_cm";
  
  // AT-команда: установить TCP-соединение с сервером
  esp.println("AT+CIPSTART=\"TCP\",\"" + PC_IP + "\",8080");
  delay(300);
  
  // Формируем HTTP GET запрос
  String get = "GET /?data=" + data + " HTTP/1.1\r\n";
  get += "Host: " + PC_IP + "\r\n";
  get += "Connection: close\r\n\r\n";
  
  // AT-команда: отправить данные (указываем длину запроса)
  esp.println("AT+CIPSEND=" + String(get.length()));
  delay(200);
  esp.print(get);          // Отправляем сам HTTP-запрос
  delay(300);
  esp.println("AT+CIPCLOSE");  // Закрываем соединение
  
  // Выводим в Serial Monitor для отладки
  Serial.print("Sent: ");
  Serial.println(data);
}

// =====================================================
//  ФУНКЦИЯ: ПОДКЛЮЧЕНИЕ К Wi-Fi ЧЕРЕЗ ESP-01S
//  Отправляет AT-команды для настройки модуля
// =====================================================
void setupESP() {
  esp.println("AT+RST");      // Перезагрузка ESP
  delay(3000);
  esp.println("AT+CWMODE=1"); // Режим станции (клиент Wi-Fi)
  delay(500);
  esp.println("AT+CWJAP=\"" + WIFI_NAME + "\",\"" + WIFI_PASS + "\""); // Подключение к сети
  delay(8000);                // Ждём подключения
  Serial.println("WiFi connected!");
}

// =====================================================
//  ФУНКЦИЯ: КАЛИБРОВКА РАССТОЯНИЯ ДО ДНА
//  Делает 20 измерений и возвращает среднее значение
// =====================================================
float calibrateBaseline() {
  Serial.println("\n=== КАЛИБРОВКА ===");
  Serial.println("Убедитесь, что ящик ПУСТ");
  
  float sum = 0;
  for (int i = 0; i < 20; i++) {
    sum += measureDistance();
    delay(100);
    Serial.print(".");   // Индикатор прогресса
  }
  Serial.println();
  
  float baseline = sum / 20.0;  // Среднее арифметическое
  Serial.print("Расстояние до дна: ");
  Serial.print(baseline);
  Serial.println(" см");
  
  return baseline;
}

// =====================================================
//  ФУНКЦИЯ: НАЧАЛЬНАЯ НАСТРОЙКА (ВЫПОЛНЯЕТСЯ 1 РАЗ)
// =====================================================
void setup() {
  Serial.begin(9600);    // Скорость для отладки через USB
  esp.begin(115200);     // Скорость для общения с ESP-01S
  
  // Настройка пинов
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  
  // Вывод информации о запуске
  Serial.println("\n========================================");
  Serial.println("     БЫСТРЫЙ ПОЧТОВЫЙ ДЕТЕКТОР v2.0");
  Serial.println("     Измерения каждые 50 мс");
  Serial.println("     Пауза после срабатывания: 7 секунд");
  Serial.println("========================================");
  
  // Подключение к Wi-Fi
  Serial.println("[1] Подключение к Wi-Fi...");
  setupESP();
  
  // Калибровка расстояния до дна
  baselineDistance = calibrateBaseline();
  
  Serial.println("\n[2] СИСТЕМА ГОТОВА!");
  Serial.println("Ожидание почты...");
  Serial.println("========================================\n");
  
  // Отправляем серверу сообщение о запуске системы
  sendToServer("SYSTEM_START", baselineDistance);
}

// =====================================================
//  ОСНОВНОЙ ЦИКЛ (ВЫПОЛНЯЕТСЯ БЕСКОНЕЧНО)
// =====================================================
void loop() {
  // === ЭТАП 1: ПРОВЕРКА ПАУЗЫ ===
  // Если прошло меньше COOLDOWN_MS с момента последнего срабатывания
  if (millis() - lastTriggerTime < COOLDOWN_MS) {
    // Режим паузы: измерения идут, но уведомления НЕ отправляются
    float dist = measureDistance();
    Serial.print(".");     // Точки показывают, что идёт пауза
    delay(MEASURE_DELAY_MS);
    return;                // Выходим из цикла, ничего не отправляем
  }
  
  // === ЭТАП 2: ИЗМЕРЕНИЕ РАССТОЯНИЯ ===
  float currentDist = measureDistance();
  
  // Ограничиваем допустимый диапазон
  if (currentDist > 30) currentDist = 30;
  if (currentDist < 0.5) currentDist = 0.5;
  
  // Проверяем, есть ли объект ближе порога
  bool objectPresent = (currentDist < OBJECT_DETECT_THRESHOLD_CM);
  
  // Вывод информации в Serial Monitor
  Serial.print("[" + String(millis() / 1000) + "s] ");
  Serial.print("Расст: ");
  Serial.print(currentDist);
  Serial.print(" см | Объект: ");
  Serial.println(objectPresent ? "ПОЧТА!" : "пусто");
  
  // === ЭТАП 3: ОБРАБОТКА СОБЫТИЙ ===
  
  // Случай 1: Объект ПОЯВИЛСЯ (было пусто, стало ПОЧТА)
  if (objectPresent && !mailPresent) {
    confirmCounter++;     // Увеличиваем счётчик подтверждений
    
    // Если подтверждений достаточно (антидребезг)
    if (confirmCounter >= DEBOUNCE_COUNT) {
      Serial.println("\n>>> ПОЧТА ОБНАРУЖЕНА! <<<\n");
      digitalWrite(LED_PIN, HIGH);      // Включаем светодиод
      
      sendToServer("MAIL|Detected", currentDist);  // Отправляем уведомление
      lastTriggerTime = millis();       // Запускаем паузу
      mailPresent = true;               // Меняем состояние
      
      delay(1000);                      // Светодиод горит 1 секунду
      digitalWrite(LED_PIN, LOW);
    }
  }
  // Случай 2: Объект ПРОПАЛ (была ПОЧТА, стало пусто)
  else if (!objectPresent && mailPresent) {
    confirmCounter++;
    
    if (confirmCounter >= DEBOUNCE_COUNT) {
      Serial.println("\n>>> ПОЧТА ЗАБРАНА <<<\n");
      sendToServer("STATUS|Mailbox_empty", currentDist);
      mailPresent = false;              // Меняем состояние
    }
  }
  // Случай 3: Состояние не изменилось - сбрасываем счётчик
  else {
    confirmCounter = 0;
  }
  
  // Задержка перед следующим измерением (50 мс)
  delay(MEASURE_DELAY_MS);
}