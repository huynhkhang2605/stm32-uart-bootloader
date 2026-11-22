import sys, time, struct
import serial

def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc

def send_packet(ser, cmd, data=b"", retries=3):
    hdr = bytes([cmd]) + struct.pack("<H", len(data))
    crc = crc16(data, crc16(hdr))
    pkt = bytes([0xAA]) + hdr + data + struct.pack("<H", crc)

    for _ in range(retries):
        ser.write(pkt)
        if ser.read(1) == b"\x06":
            return True
        time.sleep(0.2)
        ser.reset_input_buffer()
    return False

def main(port, path):
    fw = open(path, "rb").read()
    print(f"Firmware: {len(fw)} byte")

    ser = serial.Serial(port, 115200, timeout=5)
    time.sleep(0.3)
    ser.reset_input_buffer()          # bỏ chữ "BOOT" còn trong bộ đệm

    if not send_packet(ser, 0x01, struct.pack("<I", len(fw))):
        sys.exit("Loi: START (xoa flash) that bai")

    chunk = 256
    for i in range(0, len(fw), chunk):
        if not send_packet(ser, 0x02, fw[i:i + chunk]):
            sys.exit(f"Loi: goi DATA tai offset {i}")
        print(f"\r{min(i + chunk, len(fw))}/{len(fw)}", end="", flush=True)

    if not send_packet(ser, 0x03, struct.pack("<H", crc16(fw))):
        sys.exit("\nLoi: CRC toan bo firmware khong khop")

    print("\nHoan tat! Tha nut, bam Reset de chay firmware moi.")
    ser.close()

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])