# STM32 UART Bootloader

Bootloader tự viết cho **STM32F103C8T6 (Blue Pill)**, cập nhật firmware của ứng dụng qua UART mà không cần debugger. Mỗi lần truyền đều được kiểm tra bằng CRC16, và một cờ xác nhận hợp lệ (validity flag) giúp board không chạy một ứng dụng mới được cập nhật dang dở.

Viết bằng C (STM32 HAL, STM32CubeIDE) cho phía vi điều khiển và Python (pyserial) cho phía máy tính.

## Tính năng

- **Chia vùng flash:** bootloader (16 KB), ứng dụng (16 KB) và một trang riêng cho cờ xác nhận hợp lệ
- **Giao thức UART có khung gói tin:** byte mở đầu, lệnh, độ dài, dữ liệu, CRC16, kèm ACK/NACK và gửi lại
- **CRC toàn bộ firmware** tính trên đúng vùng flash đã được ghi
- **Cờ xác nhận hợp lệ:** bị xóa trước khi xóa ứng dụng, chỉ được ghi sau khi CRC cuối cùng đúng
- **Nhảy sang ứng dụng an toàn:** kiểm tra stack pointer, dời bảng vector (VTOR), đặt MSP, xử lý ngắt
- **Chế độ update:** vào bằng cách giữ nút lúc reset, hoặc tự vào khi không có ứng dụng hợp lệ
- **Tool Python** gửi file `.bin`, và một script nhỏ kiểm tra file `.bin` trước khi gửi

## Bản đồ flash (Blue Pill 64 KB)

| Địa chỉ | Kích thước | Nội dung |
|---|---|---|
| `0x08000000` | 16 KB | Bootloader (linker script giới hạn ở 16 KB) |
| `0x08004000` | 16 KB | Ứng dụng (`APP_ADDR`, `APP_MAX` = 16 KB) |
| `0x0800FC00` | 1 KB | Cờ xác nhận hợp lệ (trang cuối của chip 64 KB) |

Cờ xác nhận hợp lệ gồm ba word 32 bit: `magic` (`0xB007CAFE`), `size` (kích thước ứng dụng) và `crc` (CRC16 của ứng dụng).

## Luồng khởi động

```
Reset
  |
  v
Bootloader chạy trước (CPU luôn bắt đầu từ 0x08000000)
  |
  v
Đợi 50 ms, đọc nút ở PA0 (pull-up nội, nhấn = LOW)
  |
  +-- KHÔNG nhấn nút VÀ ứng dụng hợp lệ ------> nhảy sang 0x08004000
  |       (magic đúng, size hợp lệ, CRC16 của ứng dụng khớp với cờ)
  |
  +-- nhấn nút, HOẶC không có ứng dụng hợp lệ -> chế độ update
                                                 (in "BOOT", chờ các gói tin)
```

Board trống, ứng dụng bị hỏng, hoặc mất điện giữa lúc update đều dẫn tới chế độ update, vì cờ không còn hoặc CRC không khớp.

### Cách nhảy sang ứng dụng

Bootloader nhảy bằng phần mềm nên phải tự làm những việc mà reset cứng thường làm giúp:

1. Đọc stack pointer (word 0) và reset handler (word 1) từ bảng vector của ứng dụng.
2. Từ chối ứng dụng nếu stack pointer không nằm trong RAM.
3. Tắt ngắt và dừng SysTick.
4. Đặt `SCB->VTOR` trỏ tới bảng vector của ứng dụng.
5. Đặt `MSP` bằng stack pointer của ứng dụng.
6. Bật lại ngắt (nếu không, mặt nạ ngắt ở bước 3 vẫn còn và `HAL_Delay()` sẽ treo).
7. Gọi reset handler của ứng dụng.

## Giao thức

Mỗi yêu cầu là một frame. PC gửi một frame rồi chờ phản hồi 1 byte, sau đó mới gửi frame kế tiếp.

```
0xAA | CMD | LEN_L | LEN_H | DATA[LEN] | CRC_L | CRC_H
```

| Trường | Mô tả |
|---|---|
| `0xAA` | Byte mở đầu. Bên nhận bỏ qua mọi byte cho tới khi gặp nó |
| `CMD` | `0x01` START, `0x02` DATA, `0x03` END |
| `LEN` | 2 byte little-endian, tối đa 256 |
| `DATA` | Dữ liệu |
| `CRC` | CRC16/MODBUS (đa thức phản xạ `0xA001`, khởi đầu `0xFFFF`, không XOR cuối) trên `CMD + LEN + DATA`, gửi byte thấp trước |

Phản hồi: `0x06` = ACK, `0x15` = NACK.

| Lệnh | Dữ liệu | Bootloader làm gì |
|---|---|---|
| START (`0x01`) | Kích thước firmware, 4 byte little-endian | Xóa trang cờ trước, rồi xóa các trang ứng dụng (`ceil(size / 1024)`), rồi ACK |
| DATA (`0x02`) | Tối đa 256 byte firmware | Ghi tại địa chỉ hiện tại (ghi half-word), tăng địa chỉ, rồi ACK |
| END (`0x03`) | CRC16 của cả firmware, 2 byte little-endian | Kiểm tra số byte đã nhận, tính CRC16 trên vùng flash, nếu khớp thì xóa rồi ghi cờ hợp lệ, rồi ACK |

CRC sai, thiếu byte hoặc hết thời gian chờ đều trả NACK. Khi đó không có gì được ghi và địa chỉ không đổi. PC đợi 0,2 giây, xóa bộ đệm nhận, gửi lại đúng frame đó, tối đa 3 lần.

### Frame ví dụ (cho `firmware_samples/FW_Led_PA1.bin`, 4612 byte)

```
START :  AA 01 04 00 04 12 00 00 AB 71
DATA#1:  AA 02 00 01 | 00 50 00 20 49 43 00 08 E5 42 00 08 ... (256 byte) | A9 E6
DATA#19: AA 02 04 00 01 00 00 00 38 B8          (frame cuối, 4 byte)
END   :  AA 03 02 00 56 FB 1F 9B                (CRC16 của firmware = 0xFB56)
```

4612 byte = 18 frame đầy đủ 256 byte + 1 frame 4 byte. Tám byte dữ liệu đầu tiên là stack pointer ban đầu (`0x20005000`) và reset handler (`0x08004349`) của ứng dụng, lưu theo little-endian.

## Độ tin cậy

| Cơ chế | Bảo vệ trước điều gì |
|---|---|
| CRC16 trên mỗi frame | Dữ liệu bị sai trên đường truyền |
| ACK/NACK có gửi lại | Frame bị mất hoặc hỏng |
| PC chờ ACK rồi mới gửi frame kế | Tràn bộ đệm: USART của F103 chỉ có thanh ghi nhận 1 byte, và CPU không đọc được UART khi đang xóa hoặc ghi flash |
| CRC toàn bộ firmware tính trên vùng flash | Lỗi truyền lọt qua được, và lỗi ghi flash |
| Cờ bị xóa trước ứng dụng, chỉ ghi sau khi CRC cuối đúng | Chạy ứng dụng mới được cập nhật dang dở |
| Kiểm tra lại CRC ở mỗi lần khởi động | Ứng dụng bị hỏng sau khi đã cài |
| Kiểm tra giới hạn (kích thước, dải địa chỉ, thứ tự lệnh) | Ghi ra ngoài vùng ứng dụng |

## Phần cứng

- STM32F103C8T6 Blue Pill, ST-Link V2 để nạp bootloader lần đầu
- Mạch USB-UART (CH340N, cổng Type-C)
- Nút nhấn, LED và điện trở khoảng 300 ohm

| Tín hiệu | Chân |
|---|---|
| UART TX (nối vào RX của mạch USB-UART) | PA9 |
| UART RX (nối từ TX của mạch USB-UART) | PA10 |
| Nút nhấn (đầu còn lại nối GND) | PA0 |
| LED (ứng dụng mẫu 1) | PA1 |
| LED (ứng dụng mẫu 2) | PA2 |

Cấu hình UART: 115200, 8N1, không flow control. Nối chung GND giữa board và mạch USB-UART. Không nối chân 5 V của mạch USB-UART.

## Cấu trúc repo

```
FW_bootloader/        Project STM32CubeIDE: bootloader
FW_application/       Project STM32CubeIDE: ứng dụng mẫu (chớp LED). Tên project trong IDE là FW2
firmware_samples/     File .bin có sẵn để gửi thử
tools/
  flash_tool.py       Gửi file .bin tới bootloader qua UART
  check_bin.py        Kiểm tra file .bin có được build đúng cho bootloader này không
```

## Build và chạy

**1. Nạp bootloader một lần bằng ST-Link.** Trong cấu hình debug của CubeIDE, bỏ tick "Erase all Flash memory before download", nếu không các lần nạp sau sẽ xóa luôn các vùng khác.

**2. Build một ứng dụng dùng được với bootloader này.** Ứng dụng cần:

- dùng `FLASH ORIGIN = 0x8004000, LENGTH = 16K` trong linker script
- đặt `#define VECT_TAB_OFFSET 0x4000U` trong `system_stm32f1xx.c`
- xuất ra file nhị phân thô (Project Properties > C/C++ Build > Settings > MCU Post build outputs > Convert to binary file)

Nếu bạn sửa ứng dụng bằng CubeMX, hãy đặt code của mình giữa các cặp `USER CODE BEGIN/END`, nếu không nó sẽ bị ghi đè khi generate lại.

**3. Kiểm tra file `.bin`** (không bắt buộc nhưng nên làm):

```
python tools/check_bin.py firmware_samples/FW_Led_PA1.bin
```

```
Kich thuoc    : 4612 byte
sp            : 0x20005000
Reset_Handler : 0x08004349
CRC16 ca file : 0xFB56
sp nam trong RAM        : True
Reset_Handler o vung app: True
Bit Thumb (LSB = 1)     : True
Vua 16KB                : True
```

**4. Cập nhật firmware.**

1. Đóng mọi chương trình terminal đang giữ cổng COM.
2. Giữ nút ở PA0, bấm rồi thả reset. Bootloader lúc này ở chế độ update.
3. Chạy:

   ```
   pip install pyserial
   python tools/flash_tool.py COM4 firmware_samples/FW_Led_PA1.bin
   ```

   Thay `COM4` bằng cổng của bạn.
4. Khi tool in `Hoan tat!`, thả nút rồi bấm reset. Bootloader kiểm tra cờ và nhảy sang ứng dụng mới.

Đừng bấm reset trong lúc tool đang chạy, vì làm vậy sẽ thoát khỏi chế độ update.

## Firmware mẫu

| File | Hành vi | Kích thước | CRC16 | Trạng thái |
|---|---|---|---|---|
| `FW_Led_PA1.bin` | Chớp LED ở PA1 (1 giây) | 4612 B | `0xFB56` | Đã upload qua bootloader: mọi frame được ACK và CRC cuối được chấp nhận |
| `FW_Led_PA2.bin` | Chớp LED ở PA2 (1 giây) | 4612 B | `0xB72C` | Mới kiểm tra tĩnh (xem bên dưới), chưa chạy trên phần cứng |

Kiểm tra tĩnh cho `FW_Led_PA2.bin`: stack pointer nằm trong RAM, reset handler nằm trong vùng ứng dụng, bit Thumb được đặt, kích thước không quá 16 KB. So sánh từng byte với `FW_Led_PA1.bin` cho thấy đúng 4 byte khác nhau, khớp với việc đổi mặt nạ chân GPIO từ `0x0002` (PA1) sang `0x0004` (PA2) trong vòng chớp, và từ `0x0002` sang `0x0006` (PA1 | PA2) trong phần khởi tạo GPIO.

## Kế hoạch kiểm thử

| # | Kịch bản | Cách làm | Kỳ vọng | Kết quả |
|---|---|---|---|---|
| 1 | Update bình thường | Gửi một file `.bin` mẫu | Mọi frame được ACK, CRC cuối được chấp nhận | Upload đạt |
| 2 | Truyền bị ngắt giữa chừng | Dừng tool giữa chừng, reset không giữ nút | Bootloader in `BOOT`, ứng dụng không chạy | Chưa chạy |
| 3 | Rút dây UART giữa chừng | Rút dây, rồi reset | Giống ca 2 | Chưa chạy |
| 4 | File quá lớn | Gửi file lớn hơn 16 KB | START bị từ chối (NACK), không xóa gì | Chưa chạy |
| 5 | Frame bị sai CRC | Cho tool gửi sai CRC ở một frame | NACK, gửi lại, truyền tiếp tục | Chưa chạy |
| 6 | DATA trước START | Gửi frame DATA đầu tiên | NACK | Chưa chạy |
| 7 | CRC cả firmware sai | Cho tool gửi sai CRC trong END | END bị từ chối, cờ không được ghi | Chưa chạy |
| 8 | Ứng dụng bị hỏng sau khi cài | Dùng chương trình nạp xóa một trang của ứng dụng, rồi reset | CRC lệch lúc khởi động, vào chế độ update | Chưa chạy |
| 9 | Giữ nút khi ứng dụng hợp lệ | Giữ nút, reset | Vào chế độ update | Chưa chạy |

## Các vấn đề gặp phải khi làm

| Triệu chứng | Nguyên nhân | Cách sửa |
|---|---|---|
| Ứng dụng chạy, LED sáng rồi đứng yên | `__disable_irq()` trong bootloader để lại mặt nạ ngắt sau khi nhảy, nên SysTick không ngắt và `HAL_Delay()` không bao giờ trả về | Gọi `__enable_irq()` trước khi nhảy |
| Ngắt chạy vào handler của bootloader | `SystemInit()` của ứng dụng đặt lại `VTOR` về đầu flash | Đặt `VECT_TAB_OFFSET 0x4000U` trong ứng dụng |
| Nạp bootloader làm mất ứng dụng | "Erase all Flash" đang bật trong cấu hình debug | Tắt tùy chọn này |
| Nút đọc ra giá trị ngẫu nhiên | Chân đầu vào bị thả nổi | Bật pull-up nội |
| Bootloader có thể nhảy vào firmware mới ghi một nửa | Chỉ kiểm tra stack pointer thì đã qua ngay khi frame đầu tiên được ghi | Thêm cờ xác nhận hợp lệ và kiểm tra CRC |
| Code ứng dụng biến mất sau khi generate lại từ CubeMX | Code nằm ngoài các cặp `USER CODE` | Đặt code giữa `USER CODE BEGIN` và `END` |

## Giới hạn đã biết và hướng phát triển

- **Chưa có số thứ tự gói.** Nếu ACK bị mất sau khi frame đã được ghi, lần gửi lại sẽ bị ghi thêm một lần nữa vào địa chỉ kế tiếp.
- **Chưa có rollback.** Ứng dụng cũ bị xóa ngay khi update bắt đầu. Một vùng đệm riêng sẽ giữ bản cũ cho tới khi bản mới được kiểm tra xong.
- **Chỉ có CRC.** CRC phát hiện lỗi ngẫu nhiên nhưng không xác thực nguồn gốc firmware. Muốn xác thực thì cần chữ ký hoặc MAC.
- **Vào chế độ update cần nút nhấn** (hoặc ứng dụng thiếu/không hợp lệ). Vào từ xa chưa được làm.
- Sau khi update thành công, bootloader vẫn đứng ở chế độ update, phải reset board để chạy firmware mới.

## Yêu cầu

- STM32CubeIDE
- Python 3 và `pyserial` (`pip install pyserial`)
