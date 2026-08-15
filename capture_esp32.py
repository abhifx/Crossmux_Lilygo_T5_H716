import serial
import sys
import time

def capture():
    port = "COM5"
    baud = 115200
    try:
        ser = serial.Serial(port, baud, timeout=1)
        # Flush initial buffer
        ser.read_all()

        print("Sending SCREENSHOT command...")
        ser.write(b"CMD:SCREENSHOT\n")

        start_found = False
        size = 0
        while True:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            if line.startswith("SCREENSHOT_START:"):
                size = int(line.split(":")[1])
                print(f"Found screenshot start, size: {size}")
                start_found = True
                break

        if start_found:
            data = b""
            while len(data) < size:
                chunk = ser.read(size - len(data))
                if not chunk:
                    break
                data += chunk

            print(f"Read {len(data)} bytes")
            with open("esp32_screenshot.raw", "wb") as f:
                f.write(data)
            print("Saved to esp32_screenshot.raw")

            # Simple conversion to BMP if we know it's 1-bit or 4-bit?
            # SSD1677 is usually 1-bit or 4-bit grayscale.
            # The code says `uint8_t* buf = display.getFrameBuffer();`
            # For H716, it might be 4-bit?

        ser.close()
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    capture()
