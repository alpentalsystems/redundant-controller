"""Codec for the 64-byte controller log record (see common/include/rc/logrec.h)."""
import binascii
import struct

RECORD_SIZE = 64
MAGIC = 0x4C52
VERSION = 1
SNAPSHOT = 1
EVENT = 2

# magic, version, type, seq, io_time, mono, wall, slot, role, mode, flags,
# active_slot, io_fail, bit_results, step, mask, event, detail, reserve, crc
FMT = "<HBBIIQQBBBBBBHHBBI18xH"
FIELDS = ("type", "seq", "io_time_ms", "mono_ms", "wall_ms", "slot", "role", "mode", "flags",
          "active_slot", "io_fail", "bit_results", "step", "mask", "event", "detail")
EVENTS = {0: "", 1: "started", 2: "role_changed", 3: "healthy", 4: "unhealthy",
          5: "referee_lost", 6: "referee_back", 7: "peer_lost", 8: "peer_back",
          9: "mode_changed", 10: "fault_set", 11: "fault_cleared", 12: "bit_item",
          13: "tcp_output", 14: "slot_learned", 15: "log_dropped"}
FLAGS = ("healthy", "referee", "peer", "peer_healthy", "fault", "io_time_valid")
BIT_ITEMS = ("io_link", "loop_timing", "cross_link", "supply_voltage", "cpu_temp")
BIT_RESULTS = ("pass", "fail", "error", "not_run")
ROLES = {0: "unknown", 1: "standby", 2: "active"}
MODES = {0: "operational", 1: "test"}
SLOTS = {0: "A", 1: "B"}
ACTIONS = {1: "leds", 2: "lamp_test", 3: "run_bit"}


def crc16(data):
    """CRC-16/CCITT-FALSE, the same as rc_crc16()."""
    return binascii.crc_hqx(data, 0xFFFF)


def encode(rec):
    raw = struct.pack(FMT, MAGIC, VERSION, *(rec[k] for k in FIELDS), 0)
    return raw[:62] + struct.pack("<H", crc16(raw[:62]))


def decode(buf):
    """Returns the record as a dict, or None for a wrong size, magic, version, type, or CRC."""
    if len(buf) != RECORD_SIZE:
        return None
    v = struct.unpack(FMT, buf)
    if v[0] != MAGIC or v[1] != VERSION or v[2] not in (SNAPSHOT, EVENT):
        return None
    if v[18] != crc16(buf[:62]):
        return None
    return dict(zip(FIELDS, v[2:18]))


def decode_file(data):
    """Returns (valid records sorted by seq, number of invalid slots that are not empty)."""
    records, invalid = [], 0
    empty = bytes(RECORD_SIZE)
    for off in range(0, len(data) - RECORD_SIZE + 1, RECORD_SIZE):
        chunk = data[off:off + RECORD_SIZE]
        rec = decode(chunk)
        if rec is not None:
            records.append(rec)
        elif chunk != empty:
            invalid += 1
    records.sort(key=lambda r: r["seq"])
    return records, invalid


def flags(rec):
    return {name: bool((rec["flags"] >> i) & 1) for i, name in enumerate(FLAGS)}


def io_valid(rec):
    return bool((rec["flags"] >> FLAGS.index("io_time_valid")) & 1)


def bit_results(rec):
    return {name: BIT_RESULTS[(rec["bit_results"] >> (2 * i)) & 3]
            for i, name in enumerate(BIT_ITEMS)}


def detail_text(rec):
    event, d = EVENTS.get(rec["event"], ""), rec["detail"]
    if event == "role_changed":
        return ROLES.get(d, str(d))
    if event == "mode_changed":
        return MODES.get(d, str(d))
    if event == "slot_learned":
        return SLOTS.get(d, str(d))
    if event == "bit_item":
        item = d >> 8
        name = BIT_ITEMS[item] if item < len(BIT_ITEMS) else str(item)
        return "%s=%s" % (name, BIT_RESULTS[d & 3])
    if event == "log_dropped":
        return "%d records" % d
    if event == "tcp_output":
        action = ACTIONS.get(d >> 8, str(d >> 8))
        return "leds 0x%02x" % (d & 0xFF) if action == "leds" else action
    return ""
