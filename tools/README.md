# BootLoader PC Serial Flasher

This directory contains a minimal PC-side UART flashing tool for the bootloader project.

## Files

- `serial_flasher.py`: Python uploader using `pyserial`

## Dependency

- Python 3.9+
- `pyserial`

Install:

```bash
pip install pyserial
```

## Basic usage

```bash
python tools/serial_flasher.py --port COM3 --file .\app.bin
```

Intel HEX input:

```bash
python tools/serial_flasher.py --port COM3 --file .\app.hex
```

Flash and jump after completion:

```bash
python tools/serial_flasher.py --port COM3 --file .\app.bin --jump
```

Custom baudrate and packet size:

```bash
python tools/serial_flasher.py --port COM3 --file .\app.bin --baudrate 115200 --chunk-size 128
```

## Parameters

- `--port`: serial port name, such as `COM3`
- `--file`: application image path, supports raw `.bin` and Intel HEX `.hex`
- `--baudrate`: UART baudrate, default `115200`
- `--chunk-size`: data payload bytes per frame, default `128`
- `--app-address`: application base address, default `0x08004000`
- `--jump`: send an extra jump command after transfer
- `--timeout`: ACK timeout in seconds, default `1.0`
- `--handshake-retries`: handshake retry count, default `5`

## Serial assumptions

- UART: `USART1`
- Format: `115200 8N1`
- Application base address: `0x08004000`

These defaults come from the existing bootloader project:

- [bootloader.h](D:\project_vk\F1\BootLoader\BL\BSP\bootloader.h:8)
- [usart.c](D:\project_vk\F1\BootLoader\BL\Core\Src\usart.c:29)

## Minimal protocol in use

The PC tool now matches the MCU bootloader implementation directly. The protocol is intentionally small and optimized for sequential flashing.

### Frame format

All multi-byte fields are little-endian.

| Field | Size | Description |
| --- | ---: | --- |
| Header | 2 | Fixed `0x55 0xAA` |
| Cmd | 1 | Command code |
| Seq | 1 | Sequence number |
| Length | 2 | Payload length in bytes |
| Payload | N | Command payload |
| CRC16 | 2 | CRC16/IBM over `Cmd + Seq + Length + Payload` |

### Commands

| Cmd | Name | Direction | Payload |
| --- | --- | --- | --- |
| `0x01` | `HELLO` | Host -> MCU | empty |
| `0x02` | `START` | Host -> MCU | `<image_size:u32>` |
| `0x03` | `DATA` | Host -> MCU | `<chunk:bytes>` |
| `0x04` | `END` | Host -> MCU | `<image_crc16:u16>` |
| `0x05` | `JUMP` | Host -> MCU | empty |
| `0x80` | `ACK` | MCU -> Host | `<orig_cmd:u8><err_code:u8><state:u8>` |
| `0x81` | `NACK` | MCU -> Host | `<orig_cmd:u8><err_code:u8><state:u8>` |

### ACK and sequence rules

- The MCU replies with `ACK` or `NACK` for every host frame.
- Response `Seq` matches the request `Seq`.
- `ACK/NACK` payload byte 0 echoes the original command.
- Payload byte 1 is the MCU error code. Success is `0x00`.
- Payload byte 2 is the MCU state: `IDLE=0`, `READY=1`, `RECEIVING=2`, `FINISHED=3`.

### Handshake convention

- Host sends `HELLO(seq=0, payload=empty)`
- MCU replies `ACK(seq=0, payload=[0x01, 0x00, state])`

### Typical flow

1. Open `USART1` at `115200 8N1`
2. `HANDSHAKE`
3. `START`
4. Repeated `DATA`
5. `END`
6. Optional `JUMP`

### Minimal MCU-side expectations

To interoperate with this tool, the MCU-side protocol implementation should at least:

- parse framed packets with the format above
- validate `CRC16/IBM`
- use sequential data writes into the application area beginning at `0x08004000`
- use `START.image_size` as session metadata
- verify `END.image_crc16` against the received image bytes
- validate the application vector table before accepting `JUMP`

## Notes

- This tool intentionally avoids complex dependencies and transport logic.
- Current implementation is stop-and-wait: one frame sent, one `ACK` awaited.
- `--app-address` is kept as a guardrail and must stay `0x08004000`, which matches the current MCU build.
- Intel HEX support is intentionally strict:
  - only Intel HEX records needed for normal flashing are accepted
  - Keil-common Intel HEX `Start Linear Address Record` (`0x05`) is accepted and safely ignored
  - all data records must be at or above `0x08004000`
  - the first data record must start exactly at `0x08004000`
  - data must be contiguous; address gaps or disjoint regions are rejected instead of being hole-filled
