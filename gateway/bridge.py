import json
import logging
import queue
import os
import random

cmd_queue: queue.Queue = queue.Queue(maxsize=10)

import time
from datetime import datetime, timezone

import paho.mqtt.client as mqtt
import serial
import signal

import config

SYSLOG_LEVELS = {
    logging.CRITICAL: 2,
    logging.ERROR: 3,
    logging.WARNING: 4,
    logging.INFO: 6,
    logging.DEBUG: 7,
}

class JournalFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        prio = f"{SYSLOG_LEVELS.get(record.levelno,6)}"
        return f"<{prio}>" + super().format(record)

handler = logging.StreamHandler()
handler.setFormatter(JournalFormatter("[%(levelname)s] %(message)s"))
logging.basicConfig(
    level=logging.INFO,
    handlers=[handler]
)
log = logging.getLogger("bridge")

SETPOINT_MAX = 30.0
SETPOINT_MIN = -30.0

def apply_command(ser: serial.Serial, cmd: str) -> None:
    try:
        setpoint = float(cmd)
    except ValueError:
        log.warning(f"команда не число, отброшена: {cmd!r}")
        return

    if not (SETPOINT_MIN <= setpoint <= SETPOINT_MAX):
        log.warning(f"setpoint вне диапазона, отброшен: {setpoint}")

    frame = f"SET,setpoint={setpoint:.1f}\n"
    ser.write(frame.encode("ascii"))
    log.info(f"отправлено на ESP32: {frame.strip()}")

def xor_checksum(text: str) -> int:
    result = 0
    for char in text:
        result ^= ord(char)
    return result

def process_line(raw: bytes, last_seq: int | None) -> int | None:
    line = raw.decode(encoding="ascii", errors="replace").strip()
    if "*" not in line:
        log.warning(f"нет checksum, пропуск: {line}")
        return None, last_seq

    body, checksum_str = line.split("*", maxsplit=1)

    calculated = xor_checksum(body)
    received = int(checksum_str, 16)

    if calculated != received:
        log.warning(f"BAD checksum: тело={body} ждали={received} посчитали={calculated}")
        return None, last_seq

    parts = body.split(",")

    if parts[0] != "FRIDGE":
        log.warning(f"нет маркера FRIDGE, пропуск: {body}")
        return None, last_seq

    data = {}
    for field in parts[1:]:
        key, value = field.split("=", 1)
        data[key] = value

    try:
        temp = float(data["temp"])
        setpoint = float(data["setpoint"])
        comp = int(data["comp"])
        seq = int(data["seq"])
    except (KeyError, ValueError) as e:
        log.warning(f"битые поля, пропуск: {e}")
        return None, last_seq

    if last_seq is not None:
        gap = seq - last_seq
        if gap > 1:
            log.warning(f"ПОТЕРЯ: пропущено {gap - 1} кадров (seq {last_seq} -> {seq})")

    payload = {
        "ts": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "temp": temp,
        "setpoint": setpoint,
        "comp": comp,
        "seq": seq,
    }

    log.debug(json.dumps(payload))
    return payload, seq

running = True
buffer_pending = False

def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code != 0:
        log.error(f"MQTT connect отклонён: {reason_code}")
        return
    log.info(f"MQTT подключен как {config.CLIENT_ID}")
    client.publish(config.STATUS_TOPIC, "online", qos=1, retain=True)
    client.subscribe(config.CMD_TOPIC, qos=1)
    global buffer_pending
    buffer_pending = True
    log.info(f"подписан на {config.CMD_TOPIC}")

def on_message(client, userdata, msg):
    payload = msg.payload.decode("utf-8", errors="replace")
    try:
        cmd_queue.put_nowait(payload)
    except queue.Full:
        log.warning(f"Очередь команд переполнена, команда отброшена: {payload}")

def make_mqtt_client() -> mqtt.Client:
    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id=config.CLIENT_ID,
        clean_session=False,
    )
    client.will_set(config.STATUS_TOPIC, "offline", qos=1, retain=True)
    client.reconnect_delay_set(min_delay=1, max_delay=60)
    client.on_connect = on_connect
    client.on_message = on_message

    if config.MQTT_USER:
        client.username_pw_set(config.MQTT_USER, config.MQTT_PASSWORD)

    delay = config.RECONNECT_MIN
    while running:
        try:
            client.connect(config.BROKER, config.BROKER_PORT, keepalive=60)
            client.loop_start()
            return client
        except OSError as e:
            wait = random.uniform(0, delay)
            log.error(f"брокер недоступен: {e}, повтор через {wait:.1f}с")
            sleep_interruptible(wait)
            delay = min(delay * 2, config.RECONNECT_MAX)
    
    return None
    

def handle_shutdown(signum: int, frame) -> None:
    global running
    log.info(f"получен сигнал {signum}, завершаюсь")
    running = False

def buffer_append(line: str) -> None:
    try:
        with open(config.BUFFER_PATH, "a") as f:
            f.write(line+ "\n")
            f.flush()
            os.fsync(f.fileno())
    except OSError as e:
        log.error(f"не смог записать в буфер: {e}")

def buffer_trim() -> None:
    try:
        if os.path.getsize(config.BUFFER_PATH) <= config.BUFFER_MAX_BYTES:
            return
        with open(config.BUFFER_PATH) as f:
            lines = f.readlines()
        kept = lines[-config.BUFFER_KEEP_LINES:]
        tmp = config.BUFFER_PATH + ".tmp"
        with open(tmp, "w") as f:
            f.writelines(kept)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, config.BUFFER_PATH)
        log.warning(f"буфер обрезан: оставлено {len(kept)} из {len(lines)}")
    except OSError as e:
        log.error(f"не смог обрезать буфер: {e}")

def buffer_flush(client: mqtt.Client) -> None:
    try:
        with open(config.BUFFER_PATH) as f:
            lines = f.readlines()
    except FileNotFoundError:
        return
    except OSError as e:
        log.error(f"не смог прочитать буфер: {e}")
        return

    sent = 0
    for line in lines:
        line = line.strip()
        if not line:
            continue
        info = client.publish(config.TELEMETRY_TOPIC, line, qos=0)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            log.warning(f"отдача буфера прервана: {sent} из {len(lines)}")
            return
        sent += 1

    try:
        os.remove(config.BUFFER_PATH)
    except OSError as e:
        log.error(f"не смог удалить буфер: {e}")
    log.info(f"буфер отдан: {sent} сообщение")

def sleep_interruptible(seconds: float) -> None:
    deadline = time.monotonic() + seconds
    while running:
        left = deadline - time.monotonic()
        if left <= 0:
            return
        time.sleep(min(0.5, left))

buffering = False
def main() -> None:
    global buffer_pending
    global buffering
    last_seq = None
    signal.signal(signal.SIGTERM, handle_shutdown)
    signal.signal(signal.SIGINT, handle_shutdown)
    client = make_mqtt_client()
    if client is None:
        log.info("остановлен до подключения к брокеру")
        return
    while running:
        try:
            ser = serial.Serial(config.PORT, config.BAUD, timeout=1)
            log.info(f"Слушаю {config.PORT} на {config.BAUD} бод...")
            while running:
                raw = ser.readline()
                if raw:
                    data, last_seq = process_line(raw, last_seq)
                    if data is not None:
                        payload = json.dumps(data)
                        info = client.publish(
                            config.TELEMETRY_TOPIC,
                            payload,
                            qos=0,
                        )
                        if info.rc != mqtt.MQTT_ERR_SUCCESS:
                            if not buffering:
                                log.warning("публикация недоступна, начал буферизацию")
                                buffering = True
                            buffer_append(payload)
                            buffer_trim()
                        elif buffering:
                            buffering = False
                if buffer_pending:
                    buffer_flush(client)
                    buffer_pending = False
                try:
                    cmd = cmd_queue.get_nowait()
                except queue.Empty:
                    pass
                else:
                    apply_command(ser, cmd)
        except serial.SerialException as e:
            log.error(f"порт потерян: {e}")
            log.info("переподключение через 2с...")
            last_seq = None
            sleep_interruptible(2)
    log.info("публикую offline и отключаюсь")
    try:
        info = client.publish(config.STATUS_TOPIC, "offline", qos=1, retain=True)
        if info.rc == mqtt.MQTT_ERR_SUCCESS:
            info.wait_for_publish(timeout=2)
        else:
            log.warning(f"offline не ушел, rc={info.rc}")
    except (RuntimeError, ValueError) as e:
        log.warning(f"не смог опубликовать offline: {e}")

    client.loop_stop()
    client.disconnect()
    log.info("остановлен")

main()


