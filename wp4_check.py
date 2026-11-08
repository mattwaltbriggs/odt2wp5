#!/usr/bin/env python3
"""Structural validator for WordPerfect 4.0 for Macintosh documents.

Checks the 16-byte prefix and walks the flat gate-pair document stream,
verifying group framing, paragraph-gate pairing and the manual's fixed
Length values. This cannot prove that WordPerfect will render the file, but
it catches framing mistakes (bad lengths, unmatched gates, truncated
functions) before opening it in the app.

Usage: wp4_check.py file.wp4
Exit status: 0 = all checks passed, 1 = problems found.
"""

import sys

# Fixed Length values from the Macintosh WP 4.0 File Format Manual
# ((group, sub) -> Length). Variable-length groups are absent.
FIXED = {
    (0xC0, None): 5,   # Extended Character
    (0xC1, None): 8,   # HorzMove (tab)
    (0xC2, None): 7,   # Indent
    (0xC3, None): 4,   # Attribute ON/OFF
    (0xD0, 0x01): 20,  # Set Horizontal Margins
    (0xD0, 0x02): 12,  # Set Line Spacing
    (0xD0, 0x05): 20,  # Set Vertical Margins
    (0xD0, 0x06): 6,   # Set Justification Mode
    (0xD0, 0x08): 6,   # Set Page Number Position
    (0xD1, 0x00): 16,  # Set Text Color
    (0xD1, 0x02): 8,   # Set Font Size
    (0xD2, 0x02): 52,  # Define Paragraph Numbering
    (0xD3, 0x02): 8,   # Set Footnote Number
    (0xD3, 0x03): 8,   # Set Endnote Number
    (0xD8, 0x01): 21,  # Insert Paragraph Number
    (0xE2, 0x02): 6,   # Set Table Cell Span
}

VARIABLE_GROUPS = {0xD0, 0xD1, 0xD2, 0xD3, 0xD5, 0xD6, 0xD8, 0xE2}
EOL_TYPES = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
             0x09, 0x0A, 0x0B, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B}


def name(b):
    return "0x%02X" % b


class Checker:
    def __init__(self, data):
        self.data = data
        self.problems = []
        self.gate_open = False
        self.gate_depth = 0
        self.functions = 0

    def problem(self, off, msg):
        self.problems.append("offset 0x%04X: %s" % (off, msg))

    def check_prefix(self):
        d = self.data
        if len(d) < 16:
            self.problem(0, "file shorter than the 16-byte prefix")
            return None
        if d[0:4] != b"\xffWPC":
            self.problem(0, "bad File ID %r (expected b'\\xffWPC')" % d[0:4])
        docptr = int.from_bytes(d[4:8], "little")
        if docptr != 16:
            self.problem(4, "document pointer %d (expected 16: no embedded "
                            "resource block)" % docptr)
        product, ftype, major, minor = d[8], d[9], d[10], d[11]
        if product != 1:
            self.problem(8, "product code %d (expected 1 = pfwp)" % product)
        if ftype != 0x0A:
            self.problem(9, "file type %d (expected 10 = pfwpdoc)" % ftype)
        if major != 4:
            self.problem(10, "major version %d (expected 4)" % major)
        if minor != 0:
            self.problem(11, "minor version %d (expected 0)" % minor)
        if d[12:14] != b"\x00\x00":
            self.problem(12, "encryption key not zero")
        return docptr

    def walk(self, start):
        d = self.data
        i = start
        n = len(d)
        prev = "BOF"  # BOF | EOL | AB | other
        while i < n:
            b = d[i]
            if b == 0xAA:
                self.functions += 1
                if self.gate_open:
                    self.problem(i, "$AA while a paragraph gate is already open")
                if prev == "AB":
                    self.problem(i, "$AA directly after $AB (no end-of-line "
                                    "between); the formatter would delete it")
                elif prev not in ("BOF", "EOL"):
                    self.problem(i, "$AA does not follow an end-of-line or the "
                                    "start of the document")
                self.gate_open = True
                prev = "other"
                i += 1
            elif b == 0xAB:
                if not self.gate_open:
                    self.problem(i, "$AB without a matching $AA")
                self.gate_open = False
                prev = "AB"
                i += 1
            elif 0x20 <= b <= 0x7E or b in (0x09, 0x0A, 0x0D):
                prev = "other"
                i += 1  # text
            elif 0x80 <= b <= 0xBF:
                self.functions += 1
                # $80/$81 are the condensed hard return/page (Appendix A), so
                # a following paragraph gate is legitimately at line start.
                prev = "EOL" if b in (0x80, 0x81) else "other"
                i += 1  # self-contained single-byte function
            elif b == 0xDC:
                i = self.walk_eol(i)
                prev = "EOL"
            elif b in (0xC0, 0xC1, 0xC2, 0xC3):
                i = self.walk_fixed(i, b)
                prev = "other"
            elif b in VARIABLE_GROUPS:
                i = self.walk_variable(i, b)
                prev = "other"
            else:
                self.problem(i, "unexpected byte %s" % name(b))
                return
        if self.gate_open:
            self.problem(n, "document ends with an unclosed paragraph gate ($AA)")

    def walk_fixed(self, i, group):
        length = FIXED[(group, None)]
        if i + length > len(self.data):
            self.problem(i, "%s function truncated" % name(group))
            return len(self.data)
        if self.data[i + length - 1] != group:
            self.problem(i, "%s function does not end with its own opcode" % name(group))
        self.functions += 1
        return i + length

    def walk_eol(self, i):
        d = self.data
        if i + 6 > len(d):
            self.problem(i, "$DC function truncated")
            return len(d)
        eol_type = d[i + 1]
        length = int.from_bytes(d[i + 2:i + 4], "little")
        # Short form: <$DC><type>[Length]<type><$DC> with Length = 2.
        if eol_type not in EOL_TYPES:
            self.problem(i, "unknown $DC type 0x%02X" % eol_type)
        if length != 2:
            # Not a short form: skip a full group (Length = payload + 4).
            payload = length - 4
            end = i + 4 + payload
            if payload < 0 or end + 4 > len(d):
                self.problem(i, "$DC/%s bad Length %d" % (name(eol_type), length))
                return len(d)
            if d[end + 2] != eol_type or d[end + 3] != 0xDC:
                self.problem(i, "$DC/%s closing bytes do not match" % name(eol_type))
            self.functions += 1
            return end + 4
        if d[i + 4] != eol_type or d[i + 5] != 0xDC:
            self.problem(i, "$DC short form closing bytes do not match")
        self.functions += 1
        return i + 6

    def walk_variable(self, i, group):
        d = self.data
        if i + 4 > len(d):
            self.problem(i, "group %s truncated before its subgroup" % name(group))
            return len(d)
        sub = d[i + 1]
        length = int.from_bytes(d[i + 2:i + 4], "little")
        payload = length - 4
        end = i + 4 + payload
        if payload < 0:
            self.problem(i, "%s/%s bad Length %d" % (name(group), name(sub), length))
            return len(d)
        if end + 4 > len(d):
            self.problem(i, "%s/%s runs past end of file" % (name(group), name(sub)))
            return len(d)
        # Closing four bytes are [Length][Sub][Group]; Length is stored again.
        clen = int.from_bytes(d[end:end + 2], "little")
        if clen != length:
            self.problem(i, "%s/%s closing Length %d != opening %d"
                         % (name(group), name(sub), clen, length))
        if d[end + 2] != sub or d[end + 3] != group:
            self.problem(i, "%s/%s closing subgroup/group mismatch"
                         % (name(group), name(sub)))
        key = (group, sub)
        if key in FIXED and length != FIXED[key]:
            self.problem(i, "%s/%s Length %d != manual's fixed %d"
                         % (name(group), name(sub), length, FIXED[key]))
        if key == (0xE2, 0x01):
            self.check_table_def(i, end, payload)
        self.functions += 1
        return end + 4

    def check_table_def(self, i, end, payload):
        # $E2/$01 Table Function. With 6-byte RGB colors (three 16-bit
        # words each, per "[RGB color].3") the fields before <# of columns>
        # occupy 95 bytes; each column adds 10. libwpd's WP3 reader seeks
        # 71 bytes to <Table mode>, which confirms this layout.
        if payload < 96:
            self.problem(i, "$E2/$01 payload %d < 96-byte fixed part" % payload)
            return
        ncol = self.data[i + 4 + 95]
        expected = 96 + 10 * ncol
        if payload != expected:
            self.problem(i, "$E2/$01 payload %d != 96 + 10*%d columns = %d"
                         % (payload, ncol, expected))
        if not (1 <= ncol <= 32):
            self.problem(i, "$E2/$01 <# of columns> = %d (manual allows 1..32)"
                         % ncol)


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: wp4_check.py file.wp4\n")
        return 2
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    c = Checker(data)
    docptr = c.check_prefix()
    if docptr is not None and docptr <= len(data):
        c.walk(docptr)
    print("%s: %d bytes, %d functions, %d problem(s)"
          % (sys.argv[1], len(data), c.functions, len(c.problems)))
    for p in c.problems:
        print("  " + p)
    return 0 if not c.problems else 1


if __name__ == "__main__":
    sys.exit(main())
