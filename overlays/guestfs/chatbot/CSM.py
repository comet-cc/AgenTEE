import os
import struct
import time
from dataclasses import dataclass

# Header layout (relative to base)
SEQ_OFF = 0          # int64
ACK_OFF = 8          # int64
LEN_OFF = 16         # uint64
HDR_SIZE = 24        # bytes (SEQ+ACK+LEN)

EXIT_SIGNAL = -1
DEBUG = False  # toggle


@dataclass(frozen=True)
class MailboxLayout:
    """
    One independent mailbox region inside the device.

    Region starts at absolute offset `base`.
    Header fields are at:
      base + 0   : SEQ (int64)
      base + 8   : ACK (int64)
      base + 16  : LEN (uint64)
    Payload begins at base + payload_off and extends for (region_size - payload_off).

    IMPORTANT:
      payload_off MUST be >= 24, otherwise payload overwrites the header.
    """
    base: int = 0
    payload_off: int = 24          # <-- default is now 24
    region_size: int = 4096

    def _validate(self) -> None:
        if self.payload_off < HDR_SIZE:
            raise ValueError(
                f"Invalid payload_off={self.payload_off}. "
                f"It must be >= {HDR_SIZE} because the header uses bytes [0..{HDR_SIZE-1}]. "
                f"Use payload_off=24 (minimum) or 32 (aligned)."
            )
        if self.region_size <= self.payload_off:
            raise ValueError(
                f"Invalid region_size={self.region_size} <= payload_off={self.payload_off}."
            )
        if self.base < 0:
            raise ValueError("Invalid base: must be >= 0")

    @property
    def seq_off(self) -> int:
        return self.base + SEQ_OFF

    @property
    def ack_off(self) -> int:
        return self.base + ACK_OFF

    @property
    def len_off(self) -> int:
        return self.base + LEN_OFF

    @property
    def payload_abs_off(self) -> int:
        self._validate()
        return self.base + self.payload_off

    @property
    def capacity(self) -> int:
        self._validate()
        return self.region_size - self.payload_off


def channel_layout(
    channel_id: int,
    *,
    region_size: int = 4096,
) -> MailboxLayout:
    """
    Convenience function: channel i uses [base0 + i*region_size .. base0 + (i+1)*region_size)
    """
    return MailboxLayout(
        base=channel_id * region_size,
        payload_off=24,  # fixed payload offset to maximize capacity
        region_size=region_size,
    )


def pread_exact(fd: int, size: int, offset: int) -> bytes:
    buf = bytearray()
    while len(buf) < size:
        chunk = os.pread(fd, size - len(buf), offset + len(buf))
        if not chunk:
            raise RuntimeError("EOF or short read while reading payload")
        buf.extend(chunk)
    return bytes(buf)


def _read_q(fd: int, off: int) -> int:
    raw = os.pread(fd, 8, off)
    if len(raw) < 8:
        return 0x7FFFFFFFFFFFFFFF
    return struct.unpack("<q", raw)[0]


def _read_Q(fd: int, off: int) -> int:
    raw = os.pread(fd, 8, off)
    if len(raw) < 8:
        return 0xFFFFFFFFFFFFFFFF
    return struct.unpack("<Q", raw)[0]


def _dump_hdr(fd: int, layout: MailboxLayout, tag: str) -> None:
    if not DEBUG:
        return
    seq = _read_q(fd, layout.seq_off)
    ack = _read_q(fd, layout.ack_off)
    ln = _read_Q(fd, layout.len_off)
    # print(f"[MAILBOX base=0x{layout.base:x}] {tag}: SEQ={seq} ACK={ack} LEN={ln}")


def send_message(fd: int, layout: MailboxLayout, data: bytes, seq: int) -> None:
    cap = layout.capacity
    if len(data) > cap:
        raise ValueError(f"Data size {len(data)} exceeds capacity {cap}")

    _dump_hdr(fd, layout, f"send_message(start) seq={seq} data_len={len(data)}")

    # Write payload first
    if data:
        os.pwrite(fd, data, layout.payload_abs_off)

    # Write length
    os.pwrite(fd, struct.pack("<Q", len(data)), layout.len_off)

    # Publish seq last
    os.pwrite(fd, struct.pack("<q", seq), layout.seq_off)

    _dump_hdr(fd, layout, f"send_message(published) seq={seq}")

    spins = 0
    while True:
        ack_raw = os.pread(fd, 8, layout.ack_off)
        if len(ack_raw) < 8:
            time.sleep(0.0001)
            continue
        ack = struct.unpack("<q", ack_raw)[0]
        if ack == seq:
            _dump_hdr(fd, layout, f"send_message(acked) seq={seq}")
            break

        spins += 1
        if DEBUG and spins % 20000 == 0:
            _dump_hdr(fd, layout, f"send_message(waiting_ack) seq={seq}")
        time.sleep(0.0001)


def recv_message(fd: int, layout: MailboxLayout, expected_seq: int):
    cap = layout.capacity

    while True:
        seq_raw = os.pread(fd, 8, layout.seq_off)
        if len(seq_raw) < 8:
            time.sleep(0.0001)
            continue
        seq = struct.unpack("<q", seq_raw)[0]

        # negatives: idle/reset, except EXIT_SIGNAL
        if seq < 0:
            if seq == EXIT_SIGNAL:
                # ACK exit so sender doesn't hang
                os.pwrite(fd, struct.pack("<q", EXIT_SIGNAL), layout.ack_off)
                return None, seq
            time.sleep(0.0001)
            continue

        if seq == expected_seq:
            break

        time.sleep(0.0001)

    chunk_len_raw = os.pread(fd, 8, layout.len_off)
    if len(chunk_len_raw) < 8:
        time.sleep(0.0001)
        chunk_len_raw = os.pread(fd, 8, layout.len_off)
        if len(chunk_len_raw) < 8:
            raise RuntimeError("Failed to read chunk length")

    chunk_len = struct.unpack("<Q", chunk_len_raw)[0]
    if chunk_len > cap:
        raise ValueError(
            f"Corrupt length {chunk_len} exceeds capacity {cap} "
            f"(region_size={layout.region_size}, payload_off={layout.payload_off})"
        )

    data = pread_exact(fd, chunk_len, layout.payload_abs_off) if chunk_len else b""

    # ACK receipt
    os.pwrite(fd, struct.pack("<q", expected_seq), layout.ack_off)
    return data, seq


def CSM_receive(fd: int, layout: MailboxLayout) -> bytes:
    """
    Reassembles fragmented data from ONE mailbox layout.
    Blocks until EXIT_SIGNAL is received.
    """
    full_payload = []
    expected_seq = 0

    while True:
        data, seq = recv_message(fd, layout, expected_seq)

        if seq == EXIT_SIGNAL:
            # mark idle for this mailbox only
            os.pwrite(fd, struct.pack("<q", -2), layout.seq_off)
            break

        if data is not None:
            full_payload.append(data)
            expected_seq += 1

    return b"".join(full_payload)


def CSM_send(fd: int, layout: MailboxLayout, data: bytes) -> None:
    """
    Sends data using ONE mailbox layout.
    """
    ptr = 0
    seq = 0
    cap = layout.capacity

    if DEBUG:
        print(f"[MAILBOX base=0x{layout.base:x}] CSM_send(total={len(data)} bytes, cap={cap})")

    while ptr < len(data):
        chunk = data[ptr: ptr + cap]
        if DEBUG:
            print(f"[MAILBOX base=0x{layout.base:x}] chunk seq={seq} len={len(chunk)} ptr={ptr}")
        send_message(fd, layout, chunk, seq)
        ptr += len(chunk)
        seq += 1

    if DEBUG:
        print(f"[MAILBOX base=0x{layout.base:x}] sending EXIT_SIGNAL")
    send_message(fd, layout, b"", EXIT_SIGNAL)
