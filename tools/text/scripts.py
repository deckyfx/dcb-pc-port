"""Scenario scripts besides the city ones: the tutorial and the Fusion Shop (Andromon) events.

The US MSD scripts replace the JP ones as loose files (assets/<serial>/files/<drive>/<path>) when
they are the same program apart from text (msd.same_program with the host's show-text commands),
see docs/re/text-engine.md section 7.11:

  B:\\BETA.MSD            tutorial, host KAWSEG (tutorial_msg_show 801ED334): 0x0A cmd 0 shows
                         the text in register 0 in a box. The host fills registers 4-7 with the
                         pad buttons pressed (circle, square, triangle, cross); the US tests other
                         buttons for confirm / cancel / skip, so the JP tests are kept, and the
                         button icons in those prompts are mapped to the JP controls.
  C:\\EVENT\\UNIT0n.MSD    Fusion Shop scenes (Andromon No.1-3), host EVOSEG (unit_msd_load
                         801EB200, fusion_shop_host 801EACBC): 0x0A cmd 0 adds the text in
                         register 4 as a line of the 4-line message page (unit_msg_build 801EBF4C),
                         0x0A cmd 11 waits for circle and clears the page.

The loose C:\\EVENT\\CITYnn.MSD copies are not loaded by the game (the cities read AREAnn.PAK).
"""
from __future__ import annotations

import re
import struct
from dataclasses import dataclass

import msd as _msd

# KAWSEG tutorial host (801ED5A8): puVar[4..7] = pad bits 0x20 circle, 0x80 square, 0x10
# triangle, 0x40 cross, pressed this frame.
TUTORIAL_BUTTON_REGS = frozenset({4, 5, 6, 7})

# Icons b0 b1 b2 are circle, triangle, cross on both discs. The US confirms with cross, redraws
# (discards the hand) with triangle and skips with circle; this build confirms with circle, redraws
# with cross and skips with triangle (the JP script's tests, and its text). Attack buttons are the
# same on both discs.
TUTORIAL_BUTTONS = {b"*b2": b"*b0", b"*b1": b"*b2", b"*b0": b"*b1"}

# The US Fusion Shop text writes a double quote as the six characters \0x22, which no renderer
# decodes (the US game shows them as they are).
QUOTE = (b"\\0x22", b'"')


@dataclass(frozen=True)
class Script:
    drive: str                 # "B" -> B.DRV
    path: str                  # path inside the DRV ("EVENT/UNIT00.MSD")
    show_text: frozenset       # host (op, cmd) that only show / page text
    button_regs: frozenset = frozenset()


SCRIPTS = (
    Script("B", "BETA.MSD", frozenset({(0x0A, 0)}), TUTORIAL_BUTTON_REGS),
    *(Script("C", f"EVENT/UNIT0{n}.MSD", frozenset({(0x0A, 0), (0x0A, 0x0B)})) for n in range(3)),
)


def _jp_icons(text: bytes) -> list[bytes]:
    """The b0-b2 icon codes of a JP line (bare codes; Shift-JIS pairs skipped)."""
    out, i = [], 0
    while i < len(text):
        c = text[i]
        if 0x81 <= c <= 0x9F or 0xE0 <= c <= 0xFC:
            i += 2
            continue
        if c == ord("b") and i + 1 < len(text) and text[i + 1] in b"012":
            out.append(b"*b" + text[i + 1:i + 2])
            i += 2
            continue
        i += 1
    return out


def _us_icons(text: bytes) -> list[bytes]:
    return re.findall(rb"\*b[012]", text)


def _segments(script: bytes, show_text: frozenset) -> list[list[_msd.Record]]:
    """Text records between consecutive program (non text-side) records."""
    segs: list[list[_msd.Record]] = [[]]
    for rec in _msd.walk(script):
        if _msd._is_text_side(rec, show_text):
            if rec.op == _msd.TEXT:
                segs[-1].append(rec)
        else:
            segs.append([])
    return segs


def remap_tutorial_buttons(jp: bytes, us: bytes, show_text: frozenset) -> tuple[dict[int, bytes], list[str]]:
    """New text (by record offset in `us`) for the prompts that name confirm / redraw / skip.

    Per stretch of text between the same two program records: when the US names the same b0-b2
    icons as the JP, they are attack buttons (or match already) and stay; otherwise every line
    that is not about attacks gets TUTORIAL_BUTTONS. Returns the changes and, for the report,
    the stretches whose icons still differ from the JP (the US worded those prompts differently).
    """
    pattern = re.compile(b"|".join(re.escape(k) for k in TUTORIAL_BUTTONS))
    changes: dict[int, bytes] = {}
    notes: list[str] = []
    for n, (a, b) in enumerate(zip(_segments(jp, show_text), _segments(us, show_text))):
        jp_icons = [i for r in a for i in _jp_icons(r.text or b"")]
        if jp_icons == [i for r in b for i in _us_icons(r.text or b"")]:
            continue
        for rec in b:
            if rec.text is None or b"Attack" in rec.text or not pattern.search(rec.text):
                continue
            changes[rec.offset] = pattern.sub(lambda m: TUTORIAL_BUTTONS[m.group(0)], rec.text)
        us_icons = [i for r in b for i in _us_icons(changes.get(r.offset, r.text or b""))]
        if us_icons != jp_icons:
            notes.append(f"stretch {n}: JP {b' '.join(jp_icons).decode()} / "
                         f"US {b' '.join(us_icons).decode()}")
    return changes, notes


def graft(jp: bytes, us: bytes, spec: Script) -> tuple[bytes | None, str, list[str]]:
    """The US script, made to run on the JP host: (script, "", notes), or (None, reason, [])
    when it is not the same program as the JP one."""
    ok, why = _msd.same_program(jp, us, spec.show_text, spec.button_regs)
    if not ok:
        return None, why, []
    out = bytearray(us)
    notes: list[str] = []
    # The JP button tests (same places, same jumps: checked by same_program).
    tests = 0
    for ra, rb in zip(_msd.skeleton(jp, spec.show_text), _msd.skeleton(us, spec.show_text)):
        if ra.raw != rb.raw and _msd.is_button_test(ra, spec.button_regs):
            out[rb.offset:rb.offset + len(ra.raw)] = ra.raw
            tests += 1
    if tests:
        notes.append(f"{tests} button tests take the JP buttons")
    changes: dict[int, bytes] = {}
    if spec.button_regs:
        changes, stretches = remap_tutorial_buttons(jp, us, spec.show_text)
        notes.append(f"{len(changes)} prompts take the JP button icons")
        notes += stretches
    quotes = 0
    for rec in _msd.walk(us):
        if rec.op != _msd.TEXT or rec.text is None:
            continue
        text = changes.get(rec.offset, rec.text)
        if QUOTE[0] in text:
            quotes += text.count(QUOTE[0])
            text = text.replace(*QUOTE)
        if text == rec.text:
            continue
        # In place: the record keeps its length (a shorter string is NUL-padded), so no offset moves.
        (length,) = struct.unpack_from("<H", rec.raw, 4)
        assert len(text) <= length, "a text record cannot grow in place"
        start = rec.offset + 6
        out[start:start + length] = text.ljust(length, b"\0")
    if quotes:
        notes.append(f"{quotes} \\0x22 -> \"")
    return bytes(out), "", notes


def write_all(jp_drv, us_drv, out) -> int:
    """Graft every script of SCRIPTS: `jp_drv(drive, path)` / `us_drv(drive, path)` read a file
    from a DRV of each disc; the result goes to out/files/<drive>/<path>. A script that fails
    the check is left out (the game reads the JP one) and a stale copy removed. Returns the count."""
    written = 0
    for spec in SCRIPTS:
        dst = out / "files" / spec.drive / spec.path
        script, why, notes = graft(jp_drv(spec.drive, spec.path), us_drv(spec.drive, spec.path), spec)
        if script is None:
            print(f"script {spec.drive}:{spec.path}: kept JP ({why})")
            dst.unlink(missing_ok=True)
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(script)
        written += 1
        for line in notes:
            print(f"script {spec.drive}:{spec.path}: {line}")
    print(f"scenario scripts: {written}/{len(SCRIPTS)} with the US text -> files/")
    return written
