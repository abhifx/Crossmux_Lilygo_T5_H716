import serial
import time

def capture():
    port = "COM5"
    baud = 115200
    try:
        ser = serial.Serial(port, baud, timeout=1)
        # Toggle DTR/RTS to reset
        ser.dtr = False
        ser.rts = False
        time.sleep(0.1)
        ser.dtr = True
        ser.rts = True

        print("Capturing logs for 10 seconds...")
        start_time = time.time()
        with open("boot.log", "w", encoding="utf-8", errors="ignore") as f:
            while time.time() - start_time < 10:
                line = ser.readline().decode('utf-8', errors='ignore')
                if line:
                    print(line, end="")
                    f.write(line)
        ser.close()
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    capture()
