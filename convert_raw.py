from PIL import Image
import sys

def convert():
    width = 960
    height = 540
    # The monitor script says rotate 270 if it's raw landscape.
    # 64800 bytes is 540*960/8.

    with open("esp32_screenshot.raw", "rb") as f:
        data = f.read()

    # Try 540x960
    try:
        img = Image.frombytes("1", (960, 540), data)
        img = img.transpose(Image.ROTATE_270) # Now 540x960
        img.save("esp32_screenshot.bmp")
        print("Converted to esp32_screenshot.bmp")
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    convert()
