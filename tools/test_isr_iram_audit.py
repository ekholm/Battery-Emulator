#!/usr/bin/env python3
"""Unit tests for the ISR IRAM residency audit.

WHY THESE EXIST, when the audit is already exercised end-to-end by mutating
firmware and rebuilding: because that exercise cannot see a hole. Mutation
proves the audit CATCHES what the mutated image contains; it says nothing about
a shape the image happens not to contain. Both defects found in this walker were
of exactly that kind - a call width absent from the audited paths of the two
images anyone had run it on, and a root classification that only bites once the
compiler makes a choice it has not made yet. An audit whose verdict is "clean"
needs its parser driven by cases the firmware does not currently produce.

    python3 -m unittest discover -s tools -p 'test_*.py'

Nothing here shells out: every case drives a pure function over text of the
shape objdump and nm actually emit, so it runs anywhere with a Python and no
toolchain, no image and no board.
"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import isr_iram_audit as audit  # noqa: E402

# One IRAM window and one flash window, far enough apart that a typo cannot
# make an address land in both. The values are the shape of a real ESP32 image.
IRAM = (0x40080000, 0x40090000)
FLASH = (0x400D0000, 0x40200000)


def in_iram(a):
    return IRAM[0] <= a < IRAM[1]


def in_flash(a):
    return FLASH[0] <= a < FLASH[1]


class LineTargets(unittest.TestCase):
    """The classification every verdict rests on, one disassembly line at a time."""

    def targets(self, line, entry):
        return audit.line_targets(line, in_flash, in_iram, set(entry))

    def test_every_windowed_call_width_is_a_call(self):
        """call4/8/12 and callx4/8/12 all move control, so all six must count.

        The regex first written here was `call\\d`, which matches call4 and
        call8 and stops at the '1' of call12 - so a call12 into flash read as
        no call at all and was neither followed nor reported. That width is not
        exotic: the toolchain that builds this firmware emits 25 of them in the
        .iram0.text of esp32devkit_330.
        """
        for mnemonic in ("call0", "call4", "call8", "call12",
                         "callx0", "callx4", "callx8", "callx12"):
            line = "  40081000:\t000000\t%s\t400d1000 <victim()>" % mnemonic
            flash, iram = self.targets(line, [0x400D1000])
            self.assertEqual(flash, [0x400D1000],
                             "%s into flash was not reported" % mnemonic)

    def test_a_jump_into_flash_is_reported_too(self):
        for mnemonic in ("j", "jx"):
            line = "  40081000:\t000000\t%s\t400d1000 <victim()>" % mnemonic
            flash, _ = self.targets(line, [0x400D1000])
            self.assertEqual(flash, [0x400D1000])

    def test_a_call_within_iram_is_followed_not_reported(self):
        line = "  40081000:\t000000\tcall8\t40082000 <helper()>"
        flash, iram = self.targets(line, [0x40082000])
        self.assertEqual(flash, [])
        self.assertEqual(iram, [0x40082000])

    def test_a_literal_pool_word_that_looks_like_a_call_is_not_one(self):
        """The pool decodes as instructions; only a target that is a function
        ENTRY counts, so a plausible-looking call to mid-symbol is ignored."""
        line = "  40081000:\t000000\tcall8\t400d1004 <victim()+0x4>"
        flash, _ = self.targets(line, [0x400D1000])   # 0x400d1004 is not an entry
        self.assertEqual(flash, [])

    def test_an_l32r_loading_a_flash_code_pointer_is_reported(self):
        """binutils annotates l32r with the word it loads, so a code pointer
        taken into a register - to be jumped to through a callx later - is
        visible even though no call appears on this line."""
        line = "  40081000:\tfb8781\tl32r\ta8, 40080480 <_pool+0x7c> (400d1000 <victim()>)"
        flash, _ = self.targets(line, [0x400D1000])
        self.assertEqual(flash, [0x400D1000])

    def test_an_l32r_loading_a_dram_address_is_not_a_finding(self):
        """A data load with the cache off is fine; only instruction fetch is not."""
        line = "  40081000:\tfb8781\tl32r\ta8, 40080480 <_pool+0x7c> (3ffbe8e4 <__stack_chk_guard>)"
        flash, iram = self.targets(line, [0x3FFBE8E4])
        self.assertEqual((flash, iram), ([], []))

    def test_a_line_with_no_control_transfer_reaches_nothing(self):
        line = "  40081000:\t0288\tl32i.n\ta8, a2, 0"
        self.assertEqual(self.targets(line, [0x400D1000]), ([], []))


class Walk(unittest.TestCase):
    """The traversal, driven by a disassembler that returns canned text."""

    def build(self, functions):
        """functions: {name: (addr, [disassembly lines])} -> walk() arguments."""
        syms, by_name, text = {}, {}, {}
        for name, (addr, lines) in functions.items():
            syms[addr] = (0x40, name)
            by_name[name] = (addr, 0x40)
            text[addr] = "\n".join(lines)
        return syms, by_name, (lambda addr, size: text.get(addr, ""))

    def test_a_flash_resident_root_is_the_finding(self):
        """A root that is itself in flash used to be walked and reported clean:
        the audit assumed IRAM_ATTR on the source and never checked the image.
        """
        audit.PRESETS["_test"] = {"taken": ["root()"], "may_inline": []}
        self.addCleanup(audit.PRESETS.pop, "_test")
        syms, by_name, dis = self.build({"root()": (0x400D2000, [])})
        seen, bad, inlined = audit.walk(dis, "_test", syms, by_name, in_flash, in_iram)
        self.assertEqual([(n, d) for n, d, _ in bad], [("root()", "root()")])

    def test_the_walk_follows_iram_callees_transitively(self):
        audit.PRESETS["_test"] = {"taken": ["root()"], "may_inline": []}
        self.addCleanup(audit.PRESETS.pop, "_test")
        syms, by_name, dis = self.build({
            "root()": (0x40081000, ["  40081000:\t0\tcall8\t40082000 <mid()>"]),
            "mid()": (0x40082000, ["  40082000:\t0\tcall12\t40083000 <leaf()>"]),
            "leaf()": (0x40083000, ["  40083000:\t0\tcall8\t400d1000 <victim()>"]),
            "victim()": (0x400D1000, []),
        })
        seen, bad, _ = audit.walk(dis, "_test", syms, by_name, in_flash, in_iram)
        self.assertEqual(seen, {"root()", "mid()", "leaf()"})
        self.assertEqual([d for _, d, _ in bad], ["victim()"])

    def test_a_missing_taken_root_refuses_to_report_clean(self):
        audit.PRESETS["_test"] = {"taken": ["gone()"], "may_inline": []}
        self.addCleanup(audit.PRESETS.pop, "_test")
        syms, by_name, dis = self.build({"root()": (0x40081000, [])})
        with self.assertRaises(audit.AuditDidNotRun):
            audit.walk(dis, "_test", syms, by_name, in_flash, in_iram)

    def test_a_missing_may_inline_root_is_reported_and_does_not_fail(self):
        audit.PRESETS["_test"] = {"taken": ["root()"], "may_inline": ["helper()"]}
        self.addCleanup(audit.PRESETS.pop, "_test")
        syms, by_name, dis = self.build({"root()": (0x40081000, [])})
        seen, bad, inlined = audit.walk(dis, "_test", syms, by_name, in_flash, in_iram)
        self.assertEqual(inlined, ["helper()"])
        self.assertEqual(bad, [])


class SdkconfigCrossCheck(unittest.TestCase):
    def write(self, text):
        fd, path = tempfile.mkstemp()
        self.addCleanup(os.unlink, path)
        with os.fdopen(fd, "w") as fh:
            fh.write(text)
        return path

    def test_the_flag_set_is_read_as_set(self):
        self.assertTrue(audit.sdkconfig_flag(self.write(
            'CONFIG_IDF_TARGET_ARCH="xtensa"\nCONFIG_ARDUINO_ISR_IRAM=y\n')))

    def test_the_not_set_comment_form_is_not_a_setting(self):
        """kconfig writes an unset option as a comment, which a substring match
        would read as the option being present."""
        self.assertFalse(audit.sdkconfig_flag(self.write(
            'CONFIG_IDF_TARGET_ARCH="xtensa"\n# CONFIG_ARDUINO_ISR_IRAM is not set\n')))

    def test_a_config_with_no_arch_is_not_a_generated_sdkconfig(self):
        with self.assertRaises(audit.AuditDidNotRun):
            audit.sdkconfig_flag(self.write("CONFIG_ARDUINO_ISR_IRAM=y\n"))

    def test_the_flag_on_a_non_xtensa_target_asks_for_a_port(self):
        with self.assertRaises(audit.AuditDidNotRun):
            audit.sdkconfig_flag(self.write(
                'CONFIG_IDF_TARGET_ARCH="riscv"\nCONFIG_ARDUINO_ISR_IRAM=y\n'))


class ChipFromMap(unittest.TestCase):
    def write_map(self, body):
        d = tempfile.mkdtemp()
        elf = os.path.join(d, "firmware.elf")
        with open(os.path.join(d, "firmware.map"), "w") as fh:
            fh.write(body)
        self.addCleanup(lambda: [os.unlink(os.path.join(d, f)) for f in os.listdir(d)]
                        and os.rmdir(d))
        return elf

    def test_the_chip_comes_from_the_framework_libs_path(self):
        elf = self.write_map(
            "LOAD .../framework-arduinoespressif32-libs/esp32s3/lib/libesp_system.a\n")
        self.assertEqual(audit.target_from_map(elf), "esp32s3")

    def test_two_chips_in_one_map_is_not_a_guess_worth_making(self):
        elf = self.write_map(
            "LOAD .../framework-arduinoespressif32-libs/esp32/lib/a.a\n"
            "LOAD .../framework-arduinoespressif32-libs/esp32s3/lib/b.a\n")
        with self.assertRaises(audit.AuditDidNotRun):
            audit.target_from_map(elf)

    def test_a_map_naming_no_chip_refuses_rather_than_defaulting(self):
        """Defaulting to esp32 is what the notes-repo instruments did, and it
        mis-decodes an S3 image rather than saying so."""
        elf = self.write_map("LOAD something/else/lib.a\n")
        with self.assertRaises(audit.AuditDidNotRun):
            audit.target_from_map(elf)


if __name__ == "__main__":
    unittest.main()
