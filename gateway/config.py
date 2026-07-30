import os
import socket


# Serial (несекретное)
PORT = os.environ.get("FRIDGE_PORT","/dev/fridge")
BAUD = int(os.environ.get("FRIDGE_BAUD", "115200"))

# MQTT
CLIENT_ID = os.environ.get("FRIDGE_CLIENT_ID", socket.gethostname())
BROKER = os.environ.get("FRIDGE_BROKER", "localhost")
BROKER_PORT = int(os.environ.get("FRIDGE_BROKER_PORT", "1883"))

TELEMETRY_TOPIC = f"fridge/{CLIENT_ID}/telemetry"
STATUS_TOPIC = f"fridge/{CLIENT_ID}/status"

# CMD
CMD_TOPIC = f"fridge/{CLIENT_ID}/cmd/setpoint"

# BUFFER
BUFFER_PATH = os.environ.get("FRIDGE_BUFFER", "buffer.jsonl")
BUFFER_MAX_BYTES = int(os.environ.get("FRIDGE_BUFFER_MAX", "200000"))
BUFFER_KEEP_LINES = int(os.environ.get("FRIDGE_BUFFER_KEEP", "500"))

# RECONNECT
RECONNECT_MIN = float(os.environ.get("FRIDGE_RECONNECT_MIN", "1"))
RECONNECT_MAX = float(os.environ.get("FRIDGE_RECONNECT_MAX", "60"))
