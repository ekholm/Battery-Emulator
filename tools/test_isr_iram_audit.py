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

    def test_a_may_inline_root_is_covered_whether_or_not_it_survives(self):
        """The contract that decides where a root belongs.

        The FD library's isr() is address-taken nowhere - the trampolines are
        what ACAN2517FD::begin() registers - so it is a may_inline root. This is
        the pair of cases that says nothing is lost by classifying it that way:
        out-of-line it is walked as the trampoline's callee, and inlined away
        its body is inside the trampoline's own disassembly, which is walked
        regardless. Classified as `taken` instead, the second case is not a
        finding but a refusal to run, and the image is fine.
        """
        audit.PRESETS["_test"] = {"taken": ["tramp()"], "may_inline": ["lib_isr()"]}
        self.addCleanup(audit.PRESETS.pop, "_test")

        out_of_line = {
            "tramp()": (0x40081000, ["  40081000:\t0\tcall8\t40082000 <lib_isr()>"]),
            "lib_isr()": (0x40082000, ["  40082000:\t0\tcall8\t400d1000 <victim()>"]),
            "victim()": (0x400D1000, []),
        }
        syms, by_name, dis = self.build(out_of_line)
        seen, bad, inlined = audit.walk(dis, "_test", syms, by_name, in_flash, in_iram)
        self.assertEqual(inlined, [])
        self.assertEqual([d for _, d, _ in bad], ["victim()"])

        # the same image with the call absorbed into the trampoline
        absorbed = {
            "tramp()": (0x40081000, ["  40081000:\t0\tcall8\t400d1000 <victim()>"]),
            "victim()": (0x400D1000, []),
        }
        syms, by_name, dis = self.build(absorbed)
        seen, bad, inlined = audit.walk(dis, "_test", syms, by_name, in_flash, in_iram)
        self.assertEqual(inlined, ["lib_isr()"])
        self.assertEqual([d for _, d, _ in bad], ["victim()"],
                         "the absorbed body must still be audited")

        # and what the other classification would have done to that same image
        audit.PRESETS["_test"] = {"taken": ["tramp()", "lib_isr()"], "may_inline": []}
        with self.assertRaises(audit.AuditDidNotRun):
            audit.walk(dis, "_test", syms, by_name, in_flash, in_iram)

    def test_a_missing_may_inline_root_is_reported_and_does_not_fail(self):
        audit.PRESETS["_test"] = {"taken": ["root()"], "may_inline": ["helper()"]}
        self.addCleanup(audit.PRESETS.pop, "_test")
        syms, by_name, dis = self.build({"root()": (0x40081000, [])})
        seen, bad, inlined = audit.walk(dis, "_test", syms, by_name, in_flash, in_iram)
        self.assertEqual(inlined, ["helper()"])
        self.assertEqual(bad, [])


class PresetClassification(unittest.TestCase):
    """A change-detector, deliberately, over a table that is entirely judgement.

    `taken` and `may_inline` are not two ways of saying the same thing: a taken
    root MUST be in the image or the verdict is vacuous, and a may_inline root
    must NOT be required or a fine build fails. Which list a name belongs in is
    decided by one question - does this project hand that function's ADDRESS to
    an interrupt allocator - and the answer is in the source, where no test here
    can reach. So these pin the answers, and a change to them is meant to cost a
    re-reading of the comment above each preset rather than pass silently.
    """

    def test_the_fd_roots_are_the_two_registered_trampolines(self):
        """comm_can.cpp passes canfd_isr and canfd_2_isr to ACAN2517FD::begin();
        the library's own isr() is an ordinary call from inside them and is
        address-taken nowhere, so requiring it out-of-line would fail a build
        whose only sin was letting the compiler absorb four instructions."""
        self.assertEqual(audit.PRESETS["fd"]["taken"], ["canfd_isr()", "canfd_2_isr()"])
        self.assertIn("ACAN2517FD::isr()", audit.PRESETS["fd"]["may_inline"])

    def test_the_taken_and_may_inline_lists_never_overlap(self):
        for name, preset in audit.PRESETS.items():
            self.assertEqual(set(preset["taken"]) & set(preset["may_inline"]), set(),
                             "%s lists a root as both" % name)

    def test_every_preset_has_at_least_one_taken_root(self):
        """A preset with no taken root cannot fail vacuously-clean: there is
        nothing whose absence would tell it that it is describing the wrong
        image."""
        for name, preset in audit.PRESETS.items():
            self.assertTrue(preset["taken"], "%s would pass vacuously" % name)


class RegistrationCensus(unittest.TestCase):
    """The half of the merge gate the image cannot answer.

    A root that disappears is caught by the walk refusing to run. A root that
    APPEARS is caught by nothing: __onPinInterrupt dispatches through a table of
    function pointers, so no static walk gets from the dispatcher to a handler.
    Until this census existed, "there are exactly two attachInterrupt
    registration sites" was a sentence in sdkconfig.be_size.defaults that a
    human had to keep true.
    """

    def tree(self, files):
        root = tempfile.mkdtemp()
        for relpath, body in files.items():
            path = os.path.join(root, relpath)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as fh:
                fh.write(body)
        self.addCleanup(self.rmtree, root)
        return root

    def rmtree(self, root):
        for dirpath, dirnames, filenames in os.walk(root, topdown=False):
            for f in filenames:
                os.unlink(os.path.join(dirpath, f))
            for d in dirnames:
                os.rmdir(os.path.join(dirpath, d))
        os.rmdir(root)

    SRC = os.path.join("Software", "src")

    def test_a_call_is_a_registration(self):
        root = self.tree({os.path.join(self.SRC, "driver.cpp"):
                          "void setup() { attachInterrupt(pin, handler, FALLING); }\n"})
        self.assertEqual(audit.registration_census(root),
                         {os.path.join(self.SRC, "driver.cpp")})

    def test_the_arg_form_counts_too(self):
        root = self.tree({os.path.join(self.SRC, "driver.cpp"):
                          "void setup() { attachInterruptArg(pin, h, this, FALLING); }\n"})
        self.assertEqual(len(audit.registration_census(root)), 1)

    def test_prose_about_attachInterrupt_is_not_a_registration(self):
        """Both comment forms and a string literal. A census that counts the
        word rather than the call fails a build for explaining itself."""
        root = self.tree({os.path.join(self.SRC, "prose.cpp"): (
            "// attachInterrupt(pin, h, FALLING) is what the service binds\n"
            "/* an older draft called attachInterruptArg(pin, h, this, LOW)\n"
            "   here, which is why the comment above exists */\n"
            'const char *why = "attachInterrupt(pin, h, FALLING)";\n')})
        self.assertEqual(audit.registration_census(root), set())

    def test_a_registration_in_a_new_file_fails_the_audit_and_names_it(self):
        root = self.tree({
            os.path.join(self.SRC, "lib", "mcp2515_lite", "mcp2515_lite.cpp"):
                "attachInterruptArg(p, mcp2515_isr_handler, this, FALLING);\n",
            os.path.join(self.SRC, "lib", "pierremolinaro-ACAN2517FD", "ACAN2517FD.cpp"):
                "attachInterrupt(p, isr, FALLING);\n",
            os.path.join(self.SRC, "devboard", "newthing.cpp"):
                "attachInterrupt(p, my_new_handler, RISING);\n"})
        with self.assertRaises(audit.AuditDidNotRun) as caught:
            audit.check_registration_sites(root)
        self.assertIn("newthing.cpp", str(caught.exception))
        self.assertIn("NEW", str(caught.exception))

    def test_a_registration_that_disappeared_fails_too(self):
        """A polled drain once removed the FD registration and reboot-looped the
        board; the presets would then have been describing an interrupt that no
        longer exists."""
        root = self.tree({
            os.path.join(self.SRC, "lib", "mcp2515_lite", "mcp2515_lite.cpp"):
                "attachInterruptArg(p, mcp2515_isr_handler, this, FALLING);\n"})
        with self.assertRaises(audit.AuditDidNotRun) as caught:
            audit.check_registration_sites(root)
        self.assertIn("GONE", str(caught.exception))
        self.assertIn("ACAN2517FD.cpp", str(caught.exception))

    def test_exactly_the_expected_sites_passes(self):
        root = self.tree({
            os.path.join(self.SRC, "lib", "mcp2515_lite", "mcp2515_lite.cpp"):
                "attachInterruptArg(p, mcp2515_isr_handler, this, FALLING);\n",
            os.path.join(self.SRC, "lib", "pierremolinaro-ACAN2517FD", "ACAN2517FD.cpp"):
                "attachInterrupt(p, isr, FALLING);\n"})
        audit.check_registration_sites(root)   # must not raise

    def test_a_tree_with_no_sources_refuses_rather_than_reporting_none(self):
        """Zero registrations found in a tree that has no sources is the vacuous
        pass in its purest form: it would agree that both known sites are GONE
        for the wrong reason, or - if the expected set were empty - report a
        clean census having read nothing."""
        with self.assertRaises(audit.AuditDidNotRun):
            audit.registration_census(self.tree({}))


class ThisRepository(unittest.TestCase):
    """The census against the real tree, which is the claim CI depends on."""

    def test_the_tree_registers_interrupts_exactly_where_the_presets_say(self):
        audit.check_registration_sites(audit.REPO_ROOT)


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


class Toolchain(unittest.TestCase):
    def test_a_riscv_target_asks_for_a_port_not_for_an_install(self):
        """The chip is read before any config, so without this the first failure
        on a riscv env is a hunt for an `xtensa-esp32c3-elf-` toolchain that does
        not exist and never will."""
        for target in ("esp32c3", "esp32c6", "esp32h2", "esp32p4"):
            with self.assertRaises(audit.AuditDidNotRun) as caught:
                audit.toolchain(target)
            self.assertIn("port", str(caught.exception))

    def test_an_xtensa_target_still_looks_for_its_toolchain(self):
        for target in ("esp32", "esp32s2", "esp32s3"):
            try:
                audit.toolchain(target)
            except audit.AuditDidNotRun as exc:
                self.assertIn("toolchain under", str(exc))


class RequestVersusImage(unittest.TestCase):
    """Telling "the flag is off" apart from "the flag did not arrive".

    Both look identical in the image - the dispatcher sits in .flash.text - and
    only the first is a reason to skip. The generated per-env sdkconfig that
    would settle it is written only by a build that recompiled the framework
    libraries, i.e. it is missing on exactly the builds where those libraries
    might have come from somewhere else. The tracked fragment is always there.
    """

    def tree(self, body):
        root = tempfile.mkdtemp()
        self.addCleanup(os.rmdir, root)          # cleanups run last-registered first
        if body is not None:
            path = os.path.join(root, audit.CUSTOM_SDKCONFIG)
            with open(path, "w") as fh:
                fh.write(body)
            self.addCleanup(os.unlink, path)
        return root

    def test_the_repo_asking_for_the_flag_is_read(self):
        self.assertTrue(audit.repo_sets_flag(self.tree(
            "# a comment\nCONFIG_ARDUINO_ISR_IRAM=y\nCONFIG_OTHER=y\n")))

    def test_the_not_set_comment_form_is_not_asking(self):
        self.assertFalse(audit.repo_sets_flag(self.tree(
            "# CONFIG_ARDUINO_ISR_IRAM is not set\n")))

    def test_a_tree_without_the_fragment_answers_neither_way(self):
        """An image from somewhere else has no request to be compared against,
        and inventing one would fail every such run."""
        self.assertIsNone(audit.repo_sets_flag(self.tree(None)))

    def test_this_repository_asks_for_the_flag(self):
        self.assertTrue(audit.repo_sets_flag(audit.REPO_ROOT),
                        "%s no longer sets %s - if that is deliberate the audit's "
                        "skip path becomes reachable again" % (
                            audit.CUSTOM_SDKCONFIG, audit.FLAG))


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
