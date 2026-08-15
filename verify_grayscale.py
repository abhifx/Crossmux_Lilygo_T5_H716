import serial
import time
import sys

def send_cmd(cmd):
    port = "COM5"
    baud = 115200
    try:
        ser = serial.Serial(port, baud, timeout=1)
        print(f"Sending command: {cmd}")
        ser.write(f"CMD:{cmd}\n".encode())
        time.sleep(0.5)
        # Read response
        while ser.in_waiting:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            print(f"Response: {line}")
        ser.close()
    except Exception as e:
        print(f"Error: {e}")

def capture_gray():
    port = "COM5"
    baud = 115200
    try:
        ser = serial.Serial(port, baud, timeout=2)
        print("Sending SCREENSHOT_GRAY command...")
        ser.write(b"CMD:SCREENSHOT_GRAY\n")

        start_found = False
        size = 0
        while True:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            if line.startswith("SCREENSHOT_GRAY_START:"):
                size = int(line.split(":")[1])
                print(f"Found grayscale screenshot start, size: {size}")
                start_found = True
                break
            if line.startswith("SCREENSHOT_GRAY_FAILED"):
                print("Grayscale screenshot failed")
                break

        if start_found:
            data = b""
            while len(data) < size:
                chunk = ser.read(size - len(data))
                if not chunk:
                    break
                data += chunk

            print(f"Read {len(data)} bytes")
            with open("esp32_screenshot_gray.raw", "wb") as f:
                f.write(data)
            print("Saved to esp32_screenshot_gray.raw")

        ser.close()
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    if len(sys.argv) > 1:
        send_cmd(sys.argv[1])
    else:
        capture_gray()
