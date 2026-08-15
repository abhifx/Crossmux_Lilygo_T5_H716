import serial
import time
import sys

def capture():
    port = "COM5"
    baud = 115200
    try:
        ser = serial.Serial(port, baud, timeout=0.1)
        print(f"Opened {port}")

        # ESP32 Reset
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.5)
        ser.setDTR(True)
        ser.setRTS(False)
        time.sleep(0.1)

        ser.reset_input_buffer()

        start = time.time()
        print("--- BOOT LOG START ---")
        while time.time() - start < 20: # Capture for 20 seconds
            line = ser.readline()
            if line:
                try:
                    text = line.decode('utf-8', errors='replace')
                    sys.stdout.write(text)
                    sys.stdout.flush()
                except:
                    pass
        print("\n--- BOOT LOG END ---")
        ser.close()
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    capture()
