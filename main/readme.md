# Arduino Time Calibration - README

## 📌 專案說明
這個專案包含一個 Python 程式 `Arduino_Time_Calibration.py`，用來與 Arduino 溝通並校正時間。  
在 Linux 環境下，透過 **cron 排程**定期執行，確保 Arduino 的時間與系統時間保持一致。

---

## ⚙️ 系統設定流程

### 1. Python 程式
- 主程式：`Arduino_Time_Calibration.py`  
- 功能：
  - 讀取 Arduino 回傳的時間  
  - 與系統時間比對  
  - 若誤差超過 2 分鐘，則更新 Arduino 時間  

### 2. Linux 排程 (cron)
- 使用 `crontab -e` 新增排程：
  ```bash
  0 * * * * /usr/bin/python3 /home/weiting/Documents/Arduino_Time_Calibration/Arduino_Time_Calibration.py

這行設定代表：每小時整點執行一次。

## 🔑 常見問題與解決方式

(1) 權限問題

Arduino 裝置通常掛載在 /dev/ttyACM0，屬於 root:dialout 群組，權限為：

crw-rw---- 1 root dialout ...

一般使用者若不在 dialout 群組，會遇到「Permission denied」錯誤。

解決方法：將使用者加入 dialout 群組

sudo usermod -a -G dialout $USER

登出再登入後生效。

檢查是否已加入群組：

groups

若輸出中包含 dialout，代表已經有權限。

(2) Arduino 自動重啟問題

預設情況下，Linux 打開序列埠會觸發 Arduino reset。

解決方法：建立 udev 規則 99-arduino.rules，關閉 HUPCL。

### 1. 建立規則

- 確認 Arduino 的 VID/PID，例如：
    ```bash
    Bus 001 Device 032: ID 2341:0042 Arduino SA Mega 2560 R3 (CDC ACM)

- Vendor ID = 2341

- Product ID = 0042

### 2. 建立規則檔 /etc/udev/rules.d/99-arduino.rules：
-  
    ```bash
    # Arduino Mega 2560 R3 - Disable HUPCL to prevent auto-reset
    ACTION=="add", SUBSYSTEM=="tty", ATTRS{idVendor}=="2341", ATTRS{idProduct}=="0042", RUN+="/bin/stty -F /dev/%E -hupcl"

### 3. 重新載入規則：
- 
    ```bash
    sudo udevadm control --reload-rules
    sudo udevadm trigger

拔掉 Arduino 再插回去，檢查：

    ```bash
    stty -F /dev/ttyACM0 -a | grep hupcl

確認顯示 -hupcl 即代表設定成功。

## ✅ 總結

- Python 程式負責時間校正。

- cron 排程確保定期執行。

- 使用者必須在 dialout 群組，避免權限問題。

- 透過 99-arduino.rules 關閉 HUPCL，避免 Arduino 每次打開序列埠時自動重啟。