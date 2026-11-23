from flask import Flask, send_file, jsonify, send_from_directory
import paho.mqtt.client as mqtt
import json
import csv
import os
import time
import threading
import urllib.request
import urllib.error
import numpy as np
from datetime import datetime, timezone, timedelta

# Force IST timezone for all system and Flask console logs
os.environ['TZ'] = 'Asia/Kolkata'
time.tzset()

app = Flask(__name__)

# ============================================================
#  TELEGRAM BOT CONFIGURATION  
# ============================================================
BOT_TOKEN = "PASTE_YOUR_HTTP_API_TOKEN_HERE"
BOT_USERNAME = "PAST_YOUR_ENERGY_METER_BOT_NAME_HERE"      # <-- bot ka username ( @ ke bina ) 
BENCH_NAME = "Room 06"     
LAB_NAME   = "CCP EE"

# Alert timing policy 
ALERT_COOLDOWN_S   = 60        
FAULT_CLEAR_DELAY  = 5         

TG_API = f"https://api.telegram.org/bot{BOT_TOKEN}"
registered_chats = set()       
tg_lock = threading.Lock()

# ============================================================
#  INLINE MENU BUTTONS  (group friendly - sirf tap karo)
# ============================================================
MAIN_KEYBOARD = {
    "inline_keyboard": [
        [
            {"text": "📊 Status",     "callback_data": "cmd_status"},
            {"text": "📁 CSV Log",    "callback_data": "cmd_csv"}
        ],
        [
            {"text": "🔁 Toggle Relay", "callback_data": "cmd_relay"},
            {"text": "🔄 Reset Energy", "callback_data": "cmd_reset"}
        ],
        [
            {"text": "🔕 Unsubscribe", "callback_data": "cmd_unsubscribe"}
        ]
    ]
}

def tg_send(text, chat_id=None, keyboard=None):
    with tg_lock:
        targets = [chat_id] if chat_id is not None else list(registered_chats)
    for cid in targets:
        try:
            payload = {
                "chat_id": cid,
                "text": text,
                "parse_mode": "HTML"
            }
            if keyboard:
                payload["reply_markup"] = keyboard
            body = json.dumps(payload).encode("utf-8")
            req = urllib.request.Request(
                TG_API + "/sendMessage", data=body,
                headers={"Content-Type": "application/json"})
            urllib.request.urlopen(req, timeout=5)
        except urllib.error.HTTPError as e:
            print(f"[TG] send failed for {cid}: {e.read().decode('utf-8')}")
        except Exception as e:
            print(f"[TG] send failed for {cid}: {e}")

def tg_answer_callback(callback_id, text=""):
    try:
        body = json.dumps({
            "callback_query_id": callback_id,
            "text": text
        }).encode("utf-8")
        req = urllib.request.Request(
            TG_API + "/answerCallbackQuery", data=body,
            headers={"Content-Type": "application/json"})
        urllib.request.urlopen(req, timeout=5)
    except Exception as e:
        print(f"[TG] callback answer failed: {e}")

def tg_send_document(cid, file_path):
    if not os.path.exists(file_path):
        return

    boundary = "----TelegramBotBoundary12345"
    filename = os.path.basename(file_path)

    with open(file_path, 'rb') as f:
        file_data = f.read()

    # Build multipart/form-data payload for Telegram API using raw urllib
    body = (
        f"--{boundary}\r\n"
        f"Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
        f"{cid}\r\n"
        f"--{boundary}\r\n"
        f"Content-Disposition: form-data; name=\"document\"; filename=\"{filename}\"\r\n"
        f"Content-Type: text/csv\r\n\r\n"
    ).encode('utf-8') + file_data + f"\r\n--{boundary}--\r\n".encode('utf-8')

    req = urllib.request.Request(
        TG_API + "/sendDocument", 
        data=body,
        headers={'Content-Type': f'multipart/form-data; boundary={boundary}'}
    )

    try:
        urllib.request.urlopen(req, timeout=15)
    except urllib.error.HTTPError as e:
        print(f"[TG] document send failed for {cid}: {e.read().decode('utf-8')}")
    except Exception as e:
        print(f"[TG] document send failed for {cid}: {e}")

def tg_get_updates(offset, timeout=30):
    url = f"{TG_API}/getUpdates?offset={offset}&timeout={timeout}"
    with urllib.request.urlopen(url, timeout=timeout + 10) as r:
        return json.loads(r.read().decode("utf-8"))

# ============================================================
#  COMMAND PARSER  ( /cmd@PalashVBot  bhi kaam karega )
# ============================================================
def parse_command(text):
    """
    '/status@PalashVBot'  -> '/status'
    '/STATUS'             -> '/status'
    '/relay extra args'   -> '/relay'
    Agar command kisi AUR bot ka ho (dusra @username) -> None
    """
    parts = text.strip().split()
    if not parts or not parts[0].startswith("/"):
        return None
    raw = parts[0]
    name = raw[1:]
    if "@" in name:
        cmd, _, user = name.partition("@")
        if user.strip().lower() != BOT_USERNAME.strip().lower():
            return None          # group me dusre bot ka command - ignore
        name = cmd
    return "/" + name.lower()

# ============================================================
#  COMMAND HANDLER  (text command + button dono yahan aate hain)
# ============================================================
def handle_command(cid, name, cmd):
    if cmd in ("/start", "/register"):
        registered_chats.add(cid)
        tg_send(
            f"<b>✅ {LAB_NAME} — {BENCH_NAME}</b>\n"
            f"Hi {name}! Aap alerts yahan paayenge.\n\n"
            f"Button dabao ya command type karo:\n"
            f"/status — live V, I, P, PF\n"
            f"/csv — download CSV log file\n"
            f"/relay — relay toggle karo\n"
            f"/reset — energy counters reset\n"
            f"/unsubscribe — alerts band",
            chat_id=cid, keyboard=MAIN_KEYBOARD
        )

    elif cmd == "/menu":
        tg_send("👇 Control panel:", chat_id=cid, keyboard=MAIN_KEYBOARD)

    elif cmd == "/unsubscribe":
        registered_chats.discard(cid)
        tg_send("Unsubscribed. Dubara alerts chahiye to /start bhejo.", chat_id=cid)

    elif cmd == "/status":
        try:
            d = json.loads(latest_data)
            tg_send(
                f"<b>📊 {BENCH_NAME} Live Status</b>\n"
                f"Voltage : {d.get('voltage', 0)} V\n"
                f"Current : {d.get('current', 0)} A\n"
                f"Power   : {d.get('power', 0)} W (PF {d.get('pf', 0)})\n"
                f"Energy  : {d.get('energy', 0)} kWh → ₹{d.get('cost', 0)}\n"
                f"Relay   : {'ON' if d.get('relay') else 'OFF'}\n"
                f"Fault   : {'⚠ TRIPPED' if d.get('fault') else 'System OK'}",
                chat_id=cid
            )
        except Exception:
            tg_send("Status nahi mila — ESP32 se data nahi aa raha.", chat_id=cid)

    elif cmd == "/csv":
        if os.path.exists(CSV_FILE):
            tg_send("📁 CSV log file bhej raha hoon...", chat_id=cid)
            tg_send_document(cid, CSV_FILE)
        else:
            tg_send("❌ CSV file abhi tak nahi bani hai. Thoda data aane do.", chat_id=cid)

    elif cmd == "/relay":
        client.publish("campus/ee/meter1/cmd", "TOGGLE")
        tg_send("🔁 Relay toggle command ESP32 ko bheji.", chat_id=cid)

    elif cmd == "/reset":
        client.publish("campus/ee/meter1/cmd", "RESET")
        tg_send("🔄 Energy counters reset ho gaye (fault latch bhi clear).", chat_id=cid)

    else:
        tg_send("Samjha nahi. Button dabao ya /status, /csv, /relay, /reset, /unsubscribe try karo.",
                chat_id=cid, keyboard=MAIN_KEYBOARD)

def tg_command_loop():
    offset = 0
    print("[TG] Command listener started — /start ya button dabao")
    while True:
        try:
            for upd in tg_get_updates(offset).get("result", []):
                offset = upd["update_id"] + 1

                # ---- INLINE BUTTON TAP (callback query) ----
                cb = upd.get("callback_query")
                if cb:
                    cb_id = cb["id"]
                    msg = cb.get("message") or {}
                    cid = msg.get("chat", {}).get("id")
                    name = cb.get("from", {}).get("first_name", "Assistant")
                    data = cb.get("data", "")
                    tg_answer_callback(cb_id, "✅ Done")
                    if cid and data.startswith("cmd_"):
                        handle_command(cid, name, "/" + data[4:])
                    continue

                # ---- NORMAL TEXT MESSAGE ----
                msg = upd.get("message") or upd.get("channel_post")
                if not msg or "text" not in msg:
                    continue
                cid   = msg["chat"]["id"]
                name  = msg["chat"].get("first_name", "Assistant")
                cmd = parse_command(msg["text"])
                if cmd is None:
                    continue          # command dusre bot ka tha, ignore
                handle_command(cid, name, cmd)
        except Exception as e:
            print(f"[TG] poll error: {e} — retry in 5s")
            time.sleep(5)

tg_thread = threading.Thread(target=tg_command_loop, daemon=True)
tg_thread.start()

# ============================================================
#  DATA HANDLING & FFT
# ============================================================
data_lock = threading.Lock()
latest_data = "{}"

fft_lock = threading.Lock()
latest_fft = {"ready": False}

N    = 100        
FS   = 5000.0     
VCAL = 0.350      
FUND_BIN, H3_BIN, H5_BIN = 1, 3, 5   

CSV_FILE = "campus_power_log.csv"
if not os.path.exists(CSV_FILE):
    with open(CSV_FILE, mode='w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(["timestamp", "voltage", "current", "power", "pf", "fault"])

def log_to_csv(data):
    try:
        with open(CSV_FILE, mode='a', newline='') as f:
            writer = csv.writer(f)
            writer.writerow([
                datetime.now(timezone(timedelta(hours=5, minutes=30))).strftime('%Y-%m-%d %H:%M:%S'),
                data.get("voltage", 0),
                data.get("current", 0),
                data.get("power", 0),
                data.get("pf", 0),
                data.get("fault", False)
            ])
    except Exception:
        pass

def process_waveform(raw):
    x = np.asarray(raw, dtype=float)
    if x.size != N:
        return None
    x = x - np.mean(x)                 
    X = np.fft.rfft(x)                 
    mag = np.abs(X) * 2.0 / N          

    v1 = float(mag[FUND_BIN]) * VCAL   
    v3 = float(mag[H3_BIN])   * VCAL   
    v5 = float(mag[H5_BIN])   * VCAL   

    if v1 > 0.01:
        thd_full = float(np.sqrt(np.sum(mag[2:]**2)) * VCAL / v1 * 100.0)  
        thd_5th  = float(np.sqrt(v3**2 + v5**2) / v1 * 100.0)              
    else:
        thd_full = 0.0
        thd_5th  = 0.0

    return {
        "ready": True,
        "thd":  round(thd_full, 2),   
        "thd5": round(thd_5th, 2),
        "fund": round(v1, 3),
        "h3":   round(v3, 3),
        "h5":   round(v5, 3),
        "updated": int(time.time())
    }

# ============================================================
#  FAULT ALERT ENGINE 
# ============================================================
fault_state = {"active": False, "type": "", "since": 0.0, "last_alert": 0.0}

OC_TRIP_A = 10.0
OV_TRIP_V = 270.0

def classify_fault(data):
    if data.get("current", 0) >= OC_TRIP_A:
        return f"{data.get('current', 0):.1f}A Overcurrent", "OC"
    if data.get("voltage", 0) >= OV_TRIP_V:
        return f"{data.get('voltage', 0):.1f}V Overvoltage", "OV"
    return "Unknown fault", "UNK"

def check_fault_alert(data):
    global fault_state
    now = time.time()
    is_fault = bool(data.get("fault", False))

    if is_fault and not fault_state["active"]:
        reason, ftype = classify_fault(data)
        fault_state = {"active": True, "type": ftype, "since": now, "last_alert": now}
        tg_send(
            f"⚠️ <b>FAULT TRIPPED at {BENCH_NAME}</b>\n"
            f"🔴 {reason} detected\n"
            f"⏱ {datetime.now(timezone(timedelta(hours=5, minutes=30))).strftime('%d %b %H:%M:%S')}\n"
            f"Relay OFF — equipment protected.\n"
            f"/status — live readings\n/reset — fault clear"
        )

    elif is_fault and fault_state["active"]:
        if now - fault_state["last_alert"] >= ALERT_COOLDOWN_S:
            fault_state["last_alert"] = now
            tg_send(f"⚠️ {BENCH_NAME} fault abhi bhi ACTIVE hai. Relay tripped.")

    elif not is_fault and fault_state["active"]:
        cleared_for = now - fault_state.get("cleared_since", 0.0)
        if cleared_for >= FAULT_CLEAR_DELAY:
            dur = int(now - fault_state["since"])
            fault_state = {"active": False, "type": "", "since": 0.0, "last_alert": 0.0, "cleared_since": now}
            tg_send(f"✅ <b>{BENCH_NAME} fault cleared</b> — {dur}s baad system normal. Relay reclose ho sakta hai.")
    if not is_fault:
        fault_state["cleared_since"] = now   

# ============================================================
#  MQTT & FLASK APP
# ============================================================
def on_message(client, userdata, msg):
    global latest_data, latest_fft
    payload = msg.payload.decode()
    try:
        data = json.loads(payload)
    except json.JSONDecodeError:
        return  

    if "v_raw" in data:
        result = process_waveform(data["v_raw"])
        if result:
            with fft_lock:
                latest_fft = result
        return

    if "voltage" not in data:
        return

    with data_lock:
        latest_data = payload
    log_to_csv(data)

    try:
        check_fault_alert(data)
    except Exception as e:
        print(f"[FAULT-ALERT] error: {e}")   

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="flask-edge-server")
client.on_message = on_message
client.connect("127.0.0.1", 1883, 60)
client.subscribe("campus/ee/meter1")
client.subscribe("campus/ee/meter1/waveform")   
client.loop_start()

@app.route('/')
def index():
    return send_file('index.html')

@app.route('/data')
def get_data():
    with data_lock:
        return latest_data

@app.route('/fft')
def get_fft():
    with fft_lock:
        return jsonify(latest_fft)

@app.route('/cmd/<action>')
def send_command(action):
    client.publish("campus/ee/meter1/cmd", action)
    return "Command Sent: " + action

@app.route('/download')
def download_csv():
    if os.path.exists(CSV_FILE):
        return send_file(CSV_FILE, as_attachment=True, download_name="campus_power_log.csv")
    return "CSV file not found.", 404

@app.route('/storage/')
def list_storage():
    storage_dir = os.path.join(os.getcwd(), 'storage')
    if not os.path.exists(storage_dir):
        return "Storage folder missing.", 404

    files = os.listdir(storage_dir)
    html = "<body style='font-family: Arial; background: #1e3c72; color: white; padding: 20px;'>"
    html += "<h2>📁 Local Storage Folder</h2><ul>"
    if not files:
        html += "<li>Folder is empty</li>"
    for f in files:
        html += f"<li style='margin: 10px 0;'><a href='/storage/{f}' style='color: #00d9ff; font-size: 1.2em;' target='_blank'>{f}</a></li>"
    html += "</ul><br><button onclick='history.back()' style='padding: 8px; background: #e94560; color: white; border: none; border-radius: 8px;'>&larr; Back to Meter</button></body>"
    return html

@app.route('/storage/ofline/')
def list_offline_storage():
    storage_dir = os.path.join(os.getcwd(), 'storage', 'ofline')
    if not os.path.exists(storage_dir):
        return "Offline folder missing.", 404
    
    files = os.listdir(storage_dir)
    html = "<body style='font-family: Arial; background: #1e3c72; color: white; padding: 20px;'>"
    html += "<h2>📁 Local Storage Folder (Offline)</h2><ul>"
    if not files:
        html += "<li>Folder is empty</li>"
    for f in files:
        html += f"<li style='margin: 10px 0;'><a href='/storage/ofline/{f}' style='color: #00d9ff; font-size: 1.2em;' target='_blank'>{f}</a></li>"
    html += "</ul><br><button onclick='history.back()' style='padding: 8px; background: #e94560; color: white; border: none; border-radius: 8px;'>&larr; Back to Meter</button></body>"
    return html

@app.route('/storage/<path:filename>')
def serve_storage_file(filename):
    return send_from_directory('storage', filename)

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, threaded=True)
