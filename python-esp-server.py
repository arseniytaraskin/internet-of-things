from http.server import HTTPServer, BaseHTTPRequestHandler
import urllib.parse
import requests
import threading
import time
import sqlite3
import json
from datetime import datetime, timedelta
from collections import deque
import logging
import re

# ==================== НАСТРОЙКИ ВК ====================
VK_TOKEN = "token"                      # Токен доступа к сообществу ВК
VK_USER_ID = "434699"                   # ID пользователя для отправки уведомлений
PC_IP = "192.168.0.103"                 # IP-адрес ПК с сервером
PORT = 8080                             # Порт для приёма данных от Arduino
VK_MESSAGE_DELAY = 2.0                  # Задержка между сообщениями ВК (сек)

# Настройка логирования для отслеживания работы сервера
logging.basicConfig(
    level=logging.INFO,
    format='[%(asctime)s] %(levelname)s - %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)
logger = logging.getLogger(__name__)

# ========== БАЗА ДАННЫХ SQLITE ==========
class Database:
    """Класс для работы с базой данных SQLite. Хранит все события почтового ящика."""
    
    def __init__(self, db_path="mailbox.db"):
        self.db_path = db_path
        self.init_db()
    
    def init_db(self):
        """Создание таблицы mail_events при первом запуске."""
        with sqlite3.connect(self.db_path) as conn:
            cursor = conn.cursor()
            cursor.execute('''
                CREATE TABLE IF NOT EXISTS mail_events (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    event_type TEXT NOT NULL,
                    distance_cm REAL,
                    timestamp DATETIME DEFAULT CURRENT_TIMESTAMP
                )
            ''')
            cursor.execute('CREATE INDEX IF NOT EXISTS idx_timestamp ON mail_events(timestamp)')
            conn.commit()
            logger.info("База данных инициализирована")
    
    def add_event(self, event_type, distance_cm=None):
        """Добавление нового события в базу данных."""
        with sqlite3.connect(self.db_path) as conn:
            cursor = conn.cursor()
            cursor.execute('''
                INSERT INTO mail_events (event_type, distance_cm, timestamp)
                VALUES (?, ?, ?)
            ''', (event_type, distance_cm, datetime.now()))
            conn.commit()
            if distance_cm:
                logger.info(f"Событие добавлено в БД: {event_type}, {distance_cm} см")
            else:
                logger.info(f"Событие добавлено в БД: {event_type}")
    
    def get_last_mail_event(self):
        """Возвращает последнее событие типа MAIL_ARRIVED (последняя доставка)."""
        with sqlite3.connect(self.db_path) as conn:
            cursor = conn.cursor()
            cursor.execute('''
                SELECT event_type, distance_cm, timestamp 
                FROM mail_events 
                WHERE event_type = 'MAIL_ARRIVED'
                ORDER BY timestamp DESC 
                LIMIT 1
            ''')
            row = cursor.fetchone()
            if row:
                ts = row[2]
                if isinstance(ts, str):
                    ts = datetime.strptime(ts, '%Y-%m-%d %H:%M:%S.%f')
                return {"type": row[0], "distance": row[1], "timestamp": ts}
            return None
    
    def get_events_last_days(self, days=7):
        """Возвращает все доставки за последние N дней."""
        cutoff_date = datetime.now() - timedelta(days=days)
        with sqlite3.connect(self.db_path) as conn:
            cursor = conn.cursor()
            cursor.execute('''
                SELECT event_type, distance_cm, timestamp 
                FROM mail_events 
                WHERE event_type = 'MAIL_ARRIVED' AND timestamp >= ?
                ORDER BY timestamp DESC
            ''', (cutoff_date,))
            rows = cursor.fetchall()
            events = []
            for row in rows:
                ts = row[2]
                if isinstance(ts, str):
                    ts = datetime.strptime(ts, '%Y-%m-%d %H:%M:%S.%f')
                events.append({
                    "type": row[0],
                    "distance": row[1],
                    "timestamp": ts
                })
            return events
    
    def get_current_status(self):
        """Определяет текущее состояние ящика: есть почта или пуст."""
        with sqlite3.connect(self.db_path) as conn:
            cursor = conn.cursor()
            cursor.execute('SELECT event_type, timestamp FROM mail_events ORDER BY timestamp DESC LIMIT 1')
            last = cursor.fetchone()
            if not last:
                return "unknown", None
            last_type = last[0]
            last_time = last[1]
            if isinstance(last_time, str):
                last_time = datetime.strptime(last_time, '%Y-%m-%d %H:%M:%S.%f')
            return ("has_mail", last_time) if last_type == "MAIL_ARRIVED" else ("empty", last_time)

db = Database()

# ========== ОЧЕРЕДЬ СООБЩЕНИЙ ДЛЯ ВК ==========
class MessageQueue:
    """Очередь сообщений для отправки в ВК. Позволяет не превышать лимиты API."""
    
    def __init__(self):
        self.queue = deque()
        self.lock = threading.Lock()
        self.last_sent_time = 0
    
    def add(self, message):
        """Добавить сообщение в очередь."""
        with self.lock:
            self.queue.append(message)
            logger.info(f"Добавлено в очередь ВК")
    
    def get_next(self):
        """Извлечь следующее сообщение из очереди."""
        with self.lock:
            return self.queue.popleft() if self.queue else None

message_queue = MessageQueue()

# ========== ОТПРАВКА В ВК ==========
def send_to_vk(message):
    """Отправка сообщения пользователю через API ВКонтакте."""
    url = "https://api.vk.com/method/messages.send"
    params = {
        "user_id": VK_USER_ID,
        "message": message,
        "random_id": int(time.time() * 1000),
        "access_token": VK_TOKEN,
        "v": "5.131"
    }
    try:
        response = requests.get(url, params=params, timeout=10)
        data = response.json()
        if "error" in data:
            logger.error(f"Ошибка ВК: {data['error']['error_code']} - {data['error'].get('error_msg', '')}")
            return False
        logger.info(f"Отправлено в ВК")
        return True
    except Exception as e:
        logger.error(f"Ошибка сети: {e}")
        return False

def process_vk_queue():
    """Фоновый поток. Обрабатывает очередь сообщений с задержкой VK_MESSAGE_DELAY."""
    while True:
        msg = message_queue.get_next()
        if msg:
            if time.time() - message_queue.last_sent_time >= VK_MESSAGE_DELAY:
                if send_to_vk(msg):
                    message_queue.last_sent_time = time.time()
            else:
                time.sleep(0.5)
                message_queue.add(msg)
        time.sleep(0.1)

# ========== ФУНКЦИЯ ИЗВЛЕЧЕНИЯ ЧИСЛА ИЗ СТРОКИ ==========
def extract_distance(text):
    """Извлекает число из строки типа 'Baseline_20.00_cm' или '3.2_cm'."""
    if not text:
        return None
    match = re.search(r'(\d+\.?\d*)', str(text))
    if match:
        return float(match.group(1))
    return None

# ========== ФУНКЦИИ ДЛЯ ФОРМИРОВАНИЯ ОТВЕТОВ ==========
def get_status_response():
    """Формирует ответ на команду /status (текущее состояние ящика)."""
    status, last_time = db.get_current_status()
    if status == "has_mail":
        time_str = last_time.strftime('%d.%m.%Y %H:%M:%S') if last_time else 'Нет данных'
        return f"📬 В ящике ЕСТЬ почта!\n\nПоследнее поступление: {time_str}"
    elif status == "empty":
        time_str = last_time.strftime('%d.%m.%Y %H:%M:%S') if last_time else 'Нет данных'
        return f"📪 Ящик ПУСТ\n\nПоследний раз почту забирали: {time_str}"
    else:
        return "❓ Статус неизвестен. Нет данных о состоянии ящика."

def get_last_response():
    """Формирует ответ на команду /last (время последней доставки)."""
    last_event = db.get_last_mail_event()
    if last_event:
        return f"📮 Последняя доставка\n\n🕐 Время: {last_event['timestamp'].strftime('%d.%m.%Y %H:%M:%S')}\n📏 Расстояние: {last_event['distance']} см"
    else:
        return "❌ Нет данных о доставках почты"

def get_history_response():
    """Формирует ответ на команду /history (доставки за последние 7 дней)."""
    events = db.get_events_last_days(7)
    if events:
        text = "📋 Доставки за последние 7 дней:\n\n"
        for e in events[:15]:
            text += f"📬 {e['timestamp'].strftime('%d.%m.%Y %H:%M:%S')} - {e['distance']} см\n"
        text += f"\nВсего доставок: {len(events)}"
        return text
    else:
        return "📭 За последние 7 дней доставок не было"

def get_help_response():
    """Формирует ответ на команду /help (справка по командам)."""
    return """🤖 Доступные команды:

/status - текущее состояние ящика (есть почта или нет)
/last - информация о времени последней доставки
/history - список доставок за последние 7 дней
/help - эта справка"""

def send_demo_messages():
    """Отправка демонстрационных сообщений при запуске сервера."""
    time.sleep(3)
    
    # Добавление тестовых данных, если база пуста
    if not db.get_last_mail_event():
        logger.info("Добавление тестовых данных в БД...")
        db.add_event("MAIL_ARRIVED", 3.2)
        db.add_event("MAIL_REMOVED", None)
        db.add_event("MAIL_ARRIVED", 4.1)
        db.add_event("MAIL_ARRIVED", 2.8)
    
    status_msg = get_status_response()
    full_status_msg = "🔹 Команда /status 🔹\n\n" + status_msg
    message_queue.add(full_status_msg)
    logger.info("Отправлен ответ на /status")
    time.sleep(3)
    
    last_msg = get_last_response()
    full_last_msg = "🔹 Команда /last 🔹\n\n" + last_msg
    message_queue.add(full_last_msg)
    logger.info("Отправлен ответ на /last")
    time.sleep(3)
    
    history_msg = get_history_response()
    full_history_msg = "🔹 Команда /history 🔹\n\n" + history_msg
    message_queue.add(full_history_msg)
    logger.info("Отправлен ответ на /history")
    time.sleep(3)
    
    help_msg = get_help_response()
    full_help_msg = "🔹 Команда /help 🔹\n\n" + help_msg
    message_queue.add(full_help_msg)
    logger.info("Отправлен ответ на /help")

# ========== HTTP ОБРАБОТЧИК ДЛЯ ARDUINO ==========
class MailboxHandler(BaseHTTPRequestHandler):
    """Обрабатывает HTTP GET запросы от Arduino."""
    
    def log_message(self, format, *args):
        logger.info(f"Arduino - {format % args}")
    
    def send_response_json(self, status, data):
        """Отправляет JSON-ответ клиенту."""
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps(data).encode())
    
    def format_vk_message(self, event_type, distance, timestamp):
        """Форматирует текст сообщения для отправки в ВК в зависимости от типа события."""
        time_str = timestamp.strftime("%Y-%m-%d %H:%M:%S")
        if event_type == "MAIL_ARRIVED":
            if distance:
                return f"📬 НОВАЯ ПОЧТА!\n\n🕐 Время: {time_str}\n📏 Расстояние: {distance} см"
            else:
                return f"📬 НОВАЯ ПОЧТА!\n\nВремя: {time_str}"
        elif event_type == "MAIL_REMOVED":
            return f"📪 ПОЧТА ЗАБРАНА!\n\nВремя: {time_str}\nЯщик пуст"
        elif event_type == "SYSTEM_START":
            if distance:
                return f"🟢 СИСТЕМА ЗАПУЩЕНА\n\n🕐 Время: {time_str}\nДно ящика: {distance} см"
            else:
                return f"🟢 СИСТЕМА ЗАПУЩЕНА\n\nВремя: {time_str}"
        else:
            return f"📮 {event_type}\n\n🕐 Время: {time_str}"
    
    def do_GET(self):
        """Основной обработчик GET-запросов. Парсит данные от Arduino и сохраняет в БД."""
        try:
            parsed = urllib.parse.urlparse(self.path)
            params = urllib.parse.parse_qs(parsed.query)
            
            if 'data' not in params:
                self.send_response_json(400, {"status": "error", "message": "No data parameter"})
                return
            
            raw_data = params['data'][0]
            logger.info(f"Получено от Arduino: {raw_data}")
            now = datetime.now()
            
            distance = None
            
            # Определение типа события и извлечение расстояния
            if "SYSTEM_START" in raw_data:
                db_event_type = "SYSTEM_START"
                parts = raw_data.split('|')
                for part in parts:
                    if 'Baseline_' in part:
                        distance = extract_distance(part)
                        break
                logger.info(f"Система запущена, базовое расстояние: {distance} см")
                
            elif "MAIL|Detected" in raw_data:
                db_event_type = "MAIL_ARRIVED"
                parts = raw_data.split('|')
                for part in parts:
                    if '_cm' in part:
                        distance = extract_distance(part)
                        break
                logger.info(f"Почта обнаружена, расстояние: {distance} см")
                
            elif "Additional" in raw_data:
                db_event_type = "MAIL_ARRIVED"
                parts = raw_data.split('|')
                for part in parts:
                    if '_cm' in part:
                        distance = extract_distance(part)
                        break
                logger.info(f"Дополнительная почта, расстояние: {distance} см")
                
            elif "Mailbox_empty" in raw_data or "STATUS" in raw_data:
                db_event_type = "MAIL_REMOVED"
                logger.info("Почта забрана, ящик пуст")
                
            else:
                db_event_type = "UNKNOWN"
            
            # Сохранение события в базу данных
            db.add_event(db_event_type, distance)
            
            # Отправка уведомления в ВК (только для прибытия и удаления почты)
            if db_event_type in ["MAIL_ARRIVED", "MAIL_REMOVED"]:
                vk_message = self.format_vk_message(db_event_type, distance, now)
                message_queue.add(vk_message)
            
            self.send_response_json(200, {"status": "ok", "event": db_event_type, "distance": distance})
                
        except Exception as e:
            logger.error(f"Ошибка обработки: {e}")
            self.send_response_json(500, {"status": "error", "message": str(e)})

# ========== ЗАПУСК ==========
def run_http_server():
    """Запускает HTTP сервер для приёма данных от Arduino."""
    server_address = ('0.0.0.0', PORT)
    httpd = HTTPServer(server_address, MailboxHandler)
    logger.info(f"HTTP сервер для Arduino запущен на {PC_IP}:{PORT}")
    httpd.serve_forever()

if __name__ == "__main__":
    # Запуск HTTP сервера в отдельном потоке
    http_thread = threading.Thread(target=run_http_server, daemon=True)
    http_thread.start()
    
    # Запуск обработчика очереди ВК в отдельном потоке
    vk_queue_thread = threading.Thread(target=process_vk_queue, daemon=True)
    vk_queue_thread.start()
    
    logger.info("=" * 50)
    logger.info("ВСЕ СЕРВИСЫ ЗАПУЩЕНЫ")
    logger.info(f"Arduino -> http://{PC_IP}:{PORT}")
    logger.info("=" * 50)
    
    # Отправка демонстрационных сообщений
    send_demo_messages()
    
    # Основной поток - ожидание завершения
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        logger.info("Остановка сервера...")