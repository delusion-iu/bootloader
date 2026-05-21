#!/usr/bin/env python3
"""Minimal UART flasher for the STM32 bootloader."""

from __future__ import annotations

import argparse
import struct
import sys
import time
from pathlib import Path

FRAME_HEAD = b"\x55\xAA"

CMD_HANDSHAKE = 0x01
CMD_START = 0x02
CMD_DATA = 0x03
CMD_END = 0x04
CMD_JUMP = 0x05
CMD_ACK = 0x80
CMD_NACK = 0x81

STATUS_OK = 0x00

DEFAULT_APP_ADDRESS = 0x08004000
DEFAULT_CHUNK_SIZE = 128
DEFAULT_TIMEOUT = 1.0
READ_POLL_INTERVAL = 0.05
READ_CHUNK_SIZE = 64
ACK_PAYLOAD_LENGTH = 3

BOOT_STATE_NAMES = {
    0x00: "IDLE",
    0x01: "READY",
    0x02: "RECEIVING",
    0x03: "FINISHED",
}

ERROR_NAMES = {
    0x00: "NONE",
    0x01: "BAD_FRAME",
    0x02: "BAD_CRC",
    0x03: "BAD_LENGTH",
    0x04: "BAD_STATE",
    0x05: "BAD_SEQUENCE",
    0x06: "RANGE",
    0x07: "FLASH",
    0x08: "VERIFY",
    0x09: "APP_INVALID",
    0x0A: "NOT_READY",
}


class ProtocolError(RuntimeError):
    """Raised when the device response does not match protocol expectations."""


def crc16_ibm(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


def build_frame(cmd: int, seq: int, payload: bytes = b"") -> bytes:
    if seq < 0 or seq > 0xFF:
        raise ValueError("sequence must be in range 0..255")
    body = bytes([cmd, seq]) + struct.pack("<H", len(payload)) + payload
    crc = crc16_ibm(body)
    return FRAME_HEAD + body + struct.pack("<H", crc)


def read_exact(port, size: int) -> bytes:
    data = port.read(size)
    if len(data) != size:
        raise TimeoutError(f"expected {size} bytes, got {len(data)}")
    return data


def read_frame(
    port,
    rx_buffer: bytearray | None = None,
    deadline: float | None = None,
    max_payload_length: int | None = None,
) -> tuple[int, int, bytes]:
    if rx_buffer is None:
        rx_buffer = bytearray()
    if deadline is None:
        deadline = time.monotonic() + (port.timeout or DEFAULT_TIMEOUT)

    last_issue = "no frame header found"
    while time.monotonic() < deadline:
        head_index = rx_buffer.find(FRAME_HEAD)
        if head_index < 0:
            if len(rx_buffer) > 1:
                rx_buffer[:] = rx_buffer[-1:] if rx_buffer[-1:] == FRAME_HEAD[:1] else b""
            chunk = port.read(READ_CHUNK_SIZE)
            if chunk:
                rx_buffer.extend(chunk)
            continue

        if head_index > 0:
            del rx_buffer[:head_index]

        if len(rx_buffer) < 6:
            chunk = port.read(READ_CHUNK_SIZE)
            if chunk:
                rx_buffer.extend(chunk)
            continue

        cmd = rx_buffer[2]
        seq = rx_buffer[3]
        length = struct.unpack("<H", rx_buffer[4:6])[0]
        if max_payload_length is not None and length > max_payload_length:
            last_issue = f"invalid payload length {length}"
            del rx_buffer[0]
            continue

        frame_size = 8 + length
        if len(rx_buffer) < frame_size:
            chunk = port.read(READ_CHUNK_SIZE)
            if chunk:
                rx_buffer.extend(chunk)
            continue

        payload_end = 6 + length
        payload = bytes(rx_buffer[6:payload_end])
        recv_crc = struct.unpack("<H", rx_buffer[payload_end:frame_size])[0]
        calc_crc = crc16_ibm(bytes([cmd, seq]) + rx_buffer[4:6] + payload)
        if recv_crc != calc_crc:
            last_issue = (
                f"CRC mismatch in response: recv=0x{recv_crc:04X}, calc=0x{calc_crc:04X}"
            )
            del rx_buffer[0]
            continue

        del rx_buffer[:frame_size]
        return cmd, seq, payload

    if rx_buffer.startswith(FRAME_HEAD):
        last_issue = f"incomplete frame before timeout ({len(rx_buffer)} buffered bytes)"
    raise TimeoutError(last_issue)


def describe_state(state: int) -> str:
    return BOOT_STATE_NAMES.get(state, f"UNKNOWN(0x{state:02X})")


def describe_error(code: int) -> str:
    return ERROR_NAMES.get(code, f"UNKNOWN(0x{code:02X})")


def parse_ack_payload(payload: bytes, expected_cmd: int) -> tuple[int, int, int]:
    if len(payload) < 3:
        raise ProtocolError(f"ACK/NACK payload too short: {len(payload)}")

    orig_cmd = payload[0]
    err_code = payload[1]
    state = payload[2]

    if orig_cmd != expected_cmd:
        raise ProtocolError(
            f"response command echo mismatch: recv=0x{orig_cmd:02X}, expected=0x{expected_cmd:02X}"
        )

    return orig_cmd, err_code, state


def expect_ack(port, cmd_sent: int, seq: int, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    rx_buffer = bytearray()
    previous_timeout = port.timeout
    port.timeout = min(timeout, READ_POLL_INTERVAL)
    try:
        last_issue = "no valid ACK/NACK received"
        while time.monotonic() < deadline:
            cmd, rx_seq, payload = read_frame(
                port,
                rx_buffer=rx_buffer,
                deadline=deadline,
                max_payload_length=ACK_PAYLOAD_LENGTH,
            )
            if rx_seq != seq:
                last_issue = f"unexpected sequence: recv={rx_seq}, expected={seq}"
                continue
            if cmd == CMD_NACK:
                try:
                    _, err_code, state = parse_ack_payload(payload, cmd_sent)
                except ProtocolError as exc:
                    last_issue = str(exc)
                    continue
                raise ProtocolError(
                    f"device returned NACK: error={describe_error(err_code)} state={describe_state(state)}"
                )
            if cmd != CMD_ACK:
                last_issue = f"unexpected response command: 0x{cmd:02X}"
                continue
            try:
                _, err_code, state = parse_ack_payload(payload, cmd_sent)
            except ProtocolError as exc:
                last_issue = str(exc)
                continue
            if err_code != STATUS_OK:
                raise ProtocolError(
                    f"device returned ACK with error={describe_error(err_code)} state={describe_state(state)}"
                )
            return payload
        raise TimeoutError(last_issue)
    finally:
        port.timeout = previous_timeout


def send_command(
    port,
    cmd: int,
    seq: int,
    payload: bytes = b"",
    timeout: float = DEFAULT_TIMEOUT,
) -> bytes:
    frame = build_frame(cmd, seq, payload)
    port.write(frame)
    port.flush()
    return expect_ack(port, cmd, seq, timeout)


def chunk_iter(data: bytes, chunk_size: int):
    for offset in range(0, len(data), chunk_size):
        yield offset, data[offset : offset + chunk_size]


def next_seq(seq: int) -> int:
    return (seq + 1) & 0xFF


def make_start_payload(image: bytes) -> bytes:
    return struct.pack("<I", len(image))


def make_data_payload(chunk: bytes) -> bytes:
    return chunk


def make_end_payload(image: bytes) -> bytes:
    return struct.pack("<H", crc16_ibm(image))


def make_jump_payload() -> bytes:
    return b""


def parse_intel_hex_line(line: str, line_number: int) -> tuple[int, int, int, bytes]:
    if not line.startswith(":"):
        raise ValueError(f"line {line_number}: missing ':' record prefix")

    record = line[1:].strip()
    if len(record) < 10 or len(record) % 2 != 0:
        raise ValueError(f"line {line_number}: invalid record length")

    try:
        raw = bytes.fromhex(record)
    except ValueError as exc:
        raise ValueError(f"line {line_number}: record is not valid hexadecimal") from exc

    byte_count = raw[0]
    expected_length = 5 + byte_count
    if len(raw) != expected_length:
        raise ValueError(
            f"line {line_number}: byte count mismatch, expected {expected_length} bytes"
        )

    if sum(raw) & 0xFF:
        raise ValueError(f"line {line_number}: checksum mismatch")

    address = struct.unpack(">H", raw[1:3])[0]
    record_type = raw[3]
    data = raw[4 : 4 + byte_count]
    checksum = raw[-1]
    return address, record_type, checksum, data


def load_intel_hex(image_path: Path, app_address: int) -> bytes:
    records: list[tuple[int, bytes]] = []
    upper_address = 0
    eof_seen = False
    start_linear_address: int | None = None

    for line_number, raw_line in enumerate(image_path.read_text().splitlines(), start=1):
        line = raw_line.strip()
        if not line:
            continue

        address, record_type, _, data = parse_intel_hex_line(line, line_number)

        if record_type == 0x00:
            absolute_address = upper_address + address
            if absolute_address < app_address:
                raise ValueError(
                    f"line {line_number}: data address 0x{absolute_address:08X} is below "
                    f"app address 0x{app_address:08X}"
                )
            records.append((absolute_address, data))
        elif record_type == 0x01:
            eof_seen = True
            break
        elif record_type == 0x04:
            if len(data) != 2:
                raise ValueError(f"line {line_number}: invalid extended linear address record")
            upper_address = struct.unpack(">H", data)[0] << 16
        elif record_type == 0x05:
            if len(data) != 4:
                raise ValueError(f"line {line_number}: invalid start linear address record")
            start_linear_address = struct.unpack(">I", data)[0]
        elif record_type in (0x02, 0x03):
            raise ValueError(
                f"line {line_number}: unsupported Intel HEX record type 0x{record_type:02X}"
            )
        else:
            raise ValueError(f"line {line_number}: unknown Intel HEX record type 0x{record_type:02X}")

    if not eof_seen:
        raise ValueError("missing Intel HEX EOF record")
    if not records:
        raise ValueError("Intel HEX file contains no data records")

    records.sort(key=lambda item: item[0])

    first_address = records[0][0]
    if first_address != app_address:
        raise ValueError(
            f"Intel HEX start address 0x{first_address:08X} does not match required "
            f"app address 0x{app_address:08X}"
        )

    image = bytearray()
    expected_address = app_address
    for absolute_address, data in records:
        if absolute_address != expected_address:
            raise ValueError(
                f"Intel HEX data is not contiguous: expected 0x{expected_address:08X}, "
                f"got 0x{absolute_address:08X}"
            )
        image.extend(data)
        expected_address += len(data)

    if start_linear_address is not None:
        print(
            "[INFO] Ignoring Intel HEX start linear address "
            f"0x{start_linear_address:08X}; serial flashing uses the image data only"
        )

    return bytes(image)


def load_image(image_path: Path, app_address: int) -> bytes:
    suffix = image_path.suffix.lower()
    if suffix == ".hex":
        return load_intel_hex(image_path, app_address)
    return image_path.read_bytes()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Minimal UART flasher for BootLoader")
    parser.add_argument("--port", required=True, help="Serial port, e.g. COM3")
    parser.add_argument(
        "--file",
        required=True,
        help="Application image path (.bin or Intel HEX .hex)",
    )
    parser.add_argument("--baudrate", type=int, default=115200, help="UART baudrate")
    parser.add_argument(
        "--chunk-size",
        type=int,
        default=DEFAULT_CHUNK_SIZE,
        help="Payload bytes per data frame (recommended <= 256)",
    )
    parser.add_argument(
        "--app-address",
        type=lambda value: int(value, 0),
        default=DEFAULT_APP_ADDRESS,
        help="Target application base address",
    )
    parser.add_argument(
        "--jump",
        action="store_true",
        help="Request a jump to the application after transfer",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT,
        help="ACK timeout in seconds",
    )
    parser.add_argument(
        "--handshake-retries",
        type=int,
        default=5,
        help="Handshake retry count before giving up",
    )
    return parser.parse_args()


def validate_args(args: argparse.Namespace) -> None:
    if args.chunk_size <= 0 or args.chunk_size > 256:
        raise ValueError("chunk size must be in range 1..256")
    if args.timeout <= 0:
        raise ValueError("timeout must be positive")
    if args.app_address != DEFAULT_APP_ADDRESS:
        raise ValueError(
            f"current bootloader only supports app address 0x{DEFAULT_APP_ADDRESS:08X}"
        )


def try_handshake(port, retries: int, timeout: float) -> None:
    for attempt in range(1, retries + 1):
        try:
            payload = send_command(port, CMD_HANDSHAKE, 0, b"", timeout)
            _, _, state = parse_ack_payload(payload, CMD_HANDSHAKE)
            print(f"[OK] Handshake succeeded on attempt {attempt}")
            print(f"[INFO] Device state: {describe_state(state)}")
            return
        except (TimeoutError, ProtocolError) as exc:
            if attempt == retries:
                raise ProtocolError(f"handshake failed after {retries} attempts: {exc}") from exc
            print(f"[WARN] Handshake attempt {attempt} failed: {exc}")
            time.sleep(0.2)


def main() -> int:
    args = parse_args()

    try:
        import serial
    except ModuleNotFoundError as exc:
        raise ModuleNotFoundError(
            "pyserial is required. Install it with: pip install pyserial"
        ) from exc

    validate_args(args)

    image_path = Path(args.file)
    image = load_image(image_path, args.app_address)
    if not image:
        raise ValueError("input file is empty")

    print(f"[INFO] File: {image_path}")
    print(f"[INFO] Size: {len(image)} bytes")
    print(f"[INFO] App address: 0x{args.app_address:08X}")
    print(f"[INFO] Chunk size: {args.chunk_size} bytes")

    seq = 1
    with serial.Serial(args.port, args.baudrate, timeout=args.timeout) as port:
        port.reset_input_buffer()
        port.reset_output_buffer()

        try_handshake(port, args.handshake_retries, args.timeout)

        start_payload = make_start_payload(image)
        send_command(port, CMD_START, seq, start_payload, args.timeout)
        print("[OK] Start accepted")
        seq = next_seq(seq)

        total_chunks = (len(image) + args.chunk_size - 1) // args.chunk_size
        for index, (_, chunk) in enumerate(chunk_iter(image, args.chunk_size), start=1):
            send_command(port, CMD_DATA, seq, make_data_payload(chunk), args.timeout)
            print(f"[OK] Data {index}/{total_chunks} seq={seq} size={len(chunk)}")
            seq = next_seq(seq)

        send_command(port, CMD_END, seq, make_end_payload(image), args.timeout)
        print("[OK] End accepted")
        seq = next_seq(seq)

        if args.jump:
            send_command(port, CMD_JUMP, seq, make_jump_payload(), args.timeout)
            print("[OK] Jump accepted")

    print("[DONE] Flash sequence completed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("[ABORT] Interrupted by user", file=sys.stderr)
        sys.exit(130)
    except Exception as exc:  # pylint: disable=broad-except
        print(f"[ERROR] {exc}", file=sys.stderr)
        sys.exit(1)
