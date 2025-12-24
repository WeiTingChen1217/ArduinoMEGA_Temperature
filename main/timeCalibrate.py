import serial
import time
from datetime import datetime

# 打開序列埠 (依你的環境調整)
ser = serial.Serial('/dev/ttyACM0', 115200, timeout=2)
print("Arduino reset")
time.sleep(4)  # 等待 Arduino reset 完成
print("Arduino reset Done")
ser.reset_input_buffer()


def get_arduino_time(retries=3, delay=1):
    """向 Arduino 要時間，最多重試 3 次，回傳 datetime 物件或 None"""
    for attempt in range(retries):
        ser.reset_input_buffer()  # 清掉舊資料
        ser.write(b'GETTIME\n')
        line = ser.readline().decode().strip()

        if line.startswith("TIME "):
            try:
                return datetime.strptime(line[5:], "%Y-%m-%d %H:%M:%S")
            except ValueError:
                print(f"[Retry {attempt+1}] 解析失敗:", line)

        else:
            print(f"[Retry {attempt+1}] 未取得正確回應:", line)

        # 等待一段時間再重試
        time.sleep(delay)

    print("多次嘗試仍未取得 Arduino 時間")
    return None


def set_arduino_time(new_time: datetime, retries=3):
    """送 SETTIME 指令更新 Arduino 時間，持續讀取直到看到 '時間已更新'，失敗則重試最多 3 次"""
    cmd = f"SETTIME {new_time.strftime('%Y-%m-%d %H:%M:%S')}\n"

    for attempt in range(retries):
        ser.write(cmd.encode())
        print(f"[Retry {attempt+1}] 送出更新:", cmd.strip())

        start = time.time()
        success = False
        while time.time() - start < 1:  # 最多等 1 秒
            line = ser.readline().decode().strip()
            if line:
                print("Arduino 回覆:", line)
                if "時間已更新" in line:
                    print("✅ 成功更新時間")
                    success = True
                    break
            else:
                time.sleep(0.1)  # 每 100ms 再試一次

        if success:
            return True
        else:
            print(f"[Retry {attempt+1}] 未收到 '時間已更新'，重試中...")

    print("❌ 多次嘗試仍未成功更新時間")
    return False



def sync_time():
    """比對 Arduino 與系統時間，誤差超過 2 分鐘才更新"""
    arduino_time = get_arduino_time()
    if arduino_time:
        print("成功取得 Arduino 時間:", arduino_time)
    else:
        print("失敗，Arduino 沒回應")


    system_time = datetime.now()
    diff = abs((system_time - arduino_time).total_seconds())

    print("Arduino:", arduino_time, "System:", system_time, "Diff:", diff, "秒")


    if diff > 120:  # 超過 2 分鐘
        set_arduino_time(system_time)
    else:
        print("誤差在 2 分鐘內，不更新")

# 主程式只執行一次
if __name__ == "__main__":
    sync_time()

