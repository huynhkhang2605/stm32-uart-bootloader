import sys, struct

def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc

data = open(sys.argv[1], "rb").read()
sp, rst = struct.unpack("<II", data[:8])    # 2 ô đầu của bảng vector

print(f"Kich thuoc    : {len(data)} byte")
print(f"sp            : 0x{sp:08X}")
print(f"Reset_Handler : 0x{rst:08X}")
print(f"CRC16 ca file : 0x{crc16(data):04X}")

print("sp nam trong RAM        :", 0x20000000 < sp <= 0x20005000)
print("Reset_Handler o vung app:", 0x08004000 <= (rst & ~1) < 0x08008000)
print("Bit Thumb (LSB = 1)     :", bool(rst & 1))
print("Vua 16KB                :", len(data) <= 16 * 1024)