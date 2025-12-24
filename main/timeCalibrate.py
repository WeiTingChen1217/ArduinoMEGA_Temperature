import serial
import time
import os
import glob
from datetime import datetime

# 打開序列埠 (依你的環境調整)
ser = serial.Serial('/dev/ttyACM0', 115200, timeout=2)
print("Arduino reset")
time.sleep(4)  # 等待 Arduino reset 完成
print("Arduino reset Done")
ser.reset_input_buffer()

# ====== Log 機制 ======
LOG_DIR = "logs"

def setup_log_file():
    """建立 log 檔案，並只保留最後三個"""
    os.makedirs(LOG_DIR, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    log_file = os.path.join(LOG_DIR, f"log-{timestamp}.txt")

    # 清理舊檔，只保留最後三個
    log_files = sorted(glob.glob(os.path.join(LOG_DIR, "log-*.txt")))
    if len(log_files) >= 3:
        for old_file in log_files[:-2]:
            os.remove(old_file)

    return log_file

def write_log(log_file, message):
    """寫入 log 檔案"""
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    with open(log_file, "a") as f:
        f.write(f"[{timestamp}] {message}\n")

# ====== Arduino 時間處理 ======
def get_arduino_time(log_file, retries=3, delay=1):
    """向 Arduino 要時間，最多重試 3 次，回傳 datetime 物件或 None"""
    for attempt in range(retries):
        ser.reset_input_buffer()
        ser.write(b'GETTIME\n')
        line = ser.readline().decode().strip()

        if line.startswith("TIME "):
            try:
                arduino_time = datetime.strptime(line[5:], "%Y-%m-%d %H:%M:%S")
                write_log(log_file, f"成功取得 Arduino 時間: {arduino_time}")
                return arduino_time
            except ValueError:
                write_log(log_file, f"[Retry {attempt+1}] 解析失敗: {line}")
        else:
            write_log(log_file, f"[Retry {attempt+1}] 未取得正確回應: {line}")

        time.sleep(delay)

    write_log(log_file, "多次嘗試仍未取得 Arduino 時間")
    return None

def set_arduino_time(log_file, new_time: datetime, retries=3):
    """送 SETTIME 指令更新 Arduino 時間，持續讀取直到看到 '時間已更新'，失敗則重試最多 3 次"""
    cmd = f"SETTIME {new_time.strftime('%Y-%m-%d %H:%M:%S')}\n"

    for attempt in range(retries):
        ser.write(cmd.encode())
        write_log(log_file, f"[Retry {attempt+1}] 送出更新: {cmd.strip()}")

        start = time.time()
        success = False
        while time.time() - start < 1:
            line = ser.readline().decode().strip()
            if line:
                write_log(log_file, f"Arduino 回覆: {line}")
                if "時間已更新" in line:
                    write_log(log_file, "✅ 成功更新時間")
                    success = True
                    break
            else:
                time.sleep(0.1)

        if success:
            return True
        else:
            write_log(log_file, f"[Retry {attempt+1}] 未收到 '時間已更新'，重試中...")

    write_log(log_file, "❌ 多次嘗試仍未成功更新時間")
    return False

def sync_time(log_file):
    """比對 Arduino 與系統時間，誤差超過 2 分鐘才更新"""
    arduino_time = get_arduino_time(log_file)
    if arduino_time:
        write_log(log_file, f"成功取得 Arduino 時間: {arduino_time}")
    else:
        write_log(log_file, "失敗，Arduino 沒回應")
        return

    system_time = datetime.now()
    diff = abs((system_time - arduino_time).total_seconds())
    write_log(log_file, f"Arduino: {arduino_time}, System: {system_time}, Diff: {diff} 秒")

    if diff > 120:
        set_arduino_time(log_file, system_time)
    else:
        write_log(log_file, "誤差在 2 分鐘內，不更新")

# ====== 主程式 ======
if __name__ == "__main__":
    log_file = setup_log_file()
    write_log(log_file, "=== 開始執行 sync_time.py ===")
    sync_time(log_file)
    write_log(log_file, "=== 執行結束 ===")
    print(f"紀錄已寫入 {log_file}")