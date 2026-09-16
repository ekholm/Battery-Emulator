#!/usr/bin/env python3
"""Does anything a cache-off interrupt reaches live in .flash.text?

`CONFIG_ARDUINO_ISR_IRAM=y` (sdkconfig.be_size.defaults) makes Arduino's GPIO
dispatcher IRAM-resident and installs the GPIO ISR service with
ESP_INTR_FLAG_IRAM, so an INT arriving during an NVS commit or an OTA write is
SERVICED instead of masked until the window ends. That is only safe while every
handler on those paths - and everything the handler CALLS - is resident too.
Otherwise the interrupt that used to be merely masked becomes an instruction
fetch from a disabled flash cache, i.e. a crash instead of data loss.

The main exposure is a CALLEE - an unmarked helper somewhere down the chain, or
a header function the compiler decided to emit out-of-line into .flash.text.
That is not a source property and scanning the source cannot see it: at -Os this
codebase emitted the MCP2515 decode out-of-line on two of four envs from
identical source. So this walks the real call graph in the LINKED IMAGE.

The roots are MOSTLY safe by construction - IRAM_ATTR is a section attribute, so
a marked function cannot be emitted to flash - but "it is marked" is a fact about
the source, and in this tree the attribute sits behind an #ifdef in the vendored
FD driver. Since the image is already open, the walk checks residency of every
function it visits, roots included, rather than assuming the attribute is there.

    tools/isr_iram_audit.py .pio/build/<env>/firmware.elf [preset ...]

Presets pick the interrupt whose call graph is walked:

    dispatcher  Arduino's GPIO dispatcher __onPinInterrupt - the function the
                flag itself makes resident, above every attachInterrupt handler
    twai        the native ACAN_ESP32 (TWAI) interrupt, installed with
                esp_intr_alloc(..., ESP_INTR_FLAG_IRAM, isr, ...)
    fd          the MCP2518FD nINT path: the comm_can.cpp trampolines and
                ACAN2517FD::isr()
    mcp2515     mcp2515_lite's attachInterruptArg handler and its drain

With no preset named, every preset runs - which is what the flag's safety
condition actually asks for, since it is a property of the whole image and not
of one driver.

Only CODE matters: a literal-pool load of a DRAM address or a peripheral
register base is fine with the cache off; an instruction fetch from .flash.text
is not. So this reports only targets that land inside .flash.text - direct call
targets at any of the windowed ABI's four call widths, and l32r-loaded code
pointers that a callx then jumps to.

A walk cannot see a registration nobody wrote a preset for - the dispatcher
reaches its handlers through a table of function pointers, which nothing static
follows - so the set of files that CALL attachInterrupt is counted from the
source as well, and has to be the set the presets describe. That is the one part
of the flag's merge gate that was otherwise a sentence asking a human to
remember there are exactly two.

Exit 0 and "clean" means every call those interrupts make stays in IRAM.
Exit 1 means flash-resident code is reachable, and the symbols are named.
Exit 2 means the audit did not RUN - no toolchain, no image, no map to name the
chip, a root the image does not contain, a sdkconfig that contradicts the image,
or an attachInterrupt site no preset describes. It is a separate code from 1 on
purpose: an audit that cannot tell "found nothing" from "did not look" is not
worth running in CI.

WHETHER THE FLAG IS EVEN ON IS READ OUT OF THE IMAGE, not out of a config file,
so no env list is maintained anywhere and a new env that inherits the flag is
audited by existing. CONFIG_ARDUINO_ISR_IRAM=y is exactly what moves Arduino's
GPIO dispatcher into IRAM, so `__onPinInterrupt` landing in .iram0.text IS the
flag being in effect and it landing in .flash.text IS the flag being off - in
which case the safety condition does not apply and the audit says so and exits 0.

Reading it from the image rather than from the generated per-env sdkconfig is
deliberate, and not a shortcut: that file is written by the step that recompiles
the framework libs, so a build whose libs were already content-identical - every
cache hit, which in CI is the common case - does not produce one at all. A gate
keyed on it would fail on exactly the builds that had nothing wrong with them.
--sdkconfig is still accepted: when the file IS there it is cross-checked against
the image, and a config that disagrees with the ELF beside it fails the audit
rather than being believed.

The toolchain comes from firmware.map beside the image, which names the chip in
the framework-libs paths the linker recorded. That matters: the S3 disassembler
decodes vector opcodes the ESP32 one renders as data, so instruction boundaries
- and with them the call targets this audit extracts - depend on getting the
right one.
"""
import argparse
import os
import re
import subprocess
import sys

# Enough of the toolchain to fail early on: the walk shells out to both.
REQUIRED_TOOLS = ("objdump", "nm")
TOOLCHAIN_PACKAGE = "packages/toolchain-xtensa-esp-elf/bin"
# Espressif's riscv parts. No env here builds for one today; the point is that
# the day one does, the failure says what is actually wrong.
RISCV_TARGET_PREFIXES = ("esp32c", "esp32h", "esp32p")
# The tracked sdkconfig fragment every env inherits through platformio.ini's
# [env] custom_sdkconfig. Unlike the generated per-env file it is always in
# the tree, which is what makes it usable as the request to compare an image
# against.
CUSTOM_SDKCONFIG = "sdkconfig.be_size.defaults"

# Roots are split by whether the compiler is ALLOWED to make them disappear.
# `taken` roots are handed to the interrupt allocator by address, so the image
# must contain an out-of-line symbol for them; if one is missing, the interrupt
# is not what this audit describes and a "clean" verdict would be vacuous.
# `may_inline` roots are ordinary calls below the handler - inlined into a
# caller that IS walked, their body is still covered.
# Arduino's GPIO dispatcher: the function CONFIG_ARDUINO_ISR_IRAM itself moves,
# sitting above every attachInterrupt handler on the cache-off path. Its
# residency is also how this script decides whether the flag is in effect at all.
DISPATCHER = "__onPinInterrupt"

PRESETS = {
    "dispatcher": {
        "taken": [DISPATCHER],
        "may_inline": [],
    },
    "twai": {
        "taken": ["ACAN_ESP32::isr(void*)"],
        "may_inline": ["ACAN_ESP32::handleRXInterrupt()", "ACAN_ESP32::handleTXInterrupt()",
                       "ACAN_ESP32::getReceivedMessage(CANMessage&)",
                       "ACAN_ESP32::internalSendMessage(CANMessage const&)"],
    },
    "fd": {
        # Only the trampolines are handed over by address - comm_can.cpp passes
        # them to ACAN2517FD::begin(), which registers them with
        # attachInterrupt(). The library's isr() is an ordinary non-virtual call
        # from inside those trampolines and is address-taken nowhere, so it is a
        # may_inline root by this table's own definition and not a taken one.
        # It is out-of-line in every image built so far only because it lives in
        # another translation unit; the tree has no -flto today, and turning it
        # on would let two two-instruction trampolines absorb it and fail a
        # perfectly good build. Either way it is covered: walked as a callee of
        # canfd_isr() when it is out-of-line, and inside canfd_isr()'s own
        # disassembly when it is not.
        "taken": ["canfd_isr()", "canfd_2_isr()"],
        "may_inline": ["ACAN2517FD::isr()"],
    },
    "mcp2515": {
        "taken": ["MCP2515_Lite::mcp2515_isr_handler(void*)"],
        "may_inline": ["MCP2515_Lite::drainRx()", "MCP2515_Lite::busTryAcquireIsr()",
                       "MCP2515_Lite::busReleaseIsr()",
                       "Mcp2515IramSpi::transfer(unsigned char const*, unsigned char*, unsigned char)"],
    },
}

FLAG = "CONFIG_ARDUINO_ISR_IRAM"

# This script lives at <repo>/tools/, so the tree it audits is one level up -
# resolved from __file__ rather than from the working directory, because CI
# runs it from the repo root and a developer runs it from wherever they are.
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# THE HALF OF THE CONDITION THE IMAGE CANNOT SHOW YOU.
#
# The presets above describe how interrupts are registered TODAY, and a root
# that disappears is caught: the audit refuses to run rather than walking
# nothing. A root that APPEARS is not. `__onPinInterrupt` dispatches through a
# table of function pointers, so a static walk cannot get from the dispatcher to
# a handler, and a new attachInterrupt() somewhere in the tree is therefore
# walked by nothing at all while CI stays green - which is the one part of the
# merge gate that was still a sentence in sdkconfig.be_size.defaults asking a
# human to remember that there are exactly two registration sites.
#
# So the sites are counted from the source instead. This is not a call-graph
# claim and does not pretend to be: it asks only whether the set of files that
# register a GPIO interrupt is still the set the presets describe. A new one
# fails the audit and names itself, and whoever added it either adds a preset
# for the handler or says here why the handler needs none.
SOURCE_TREE = os.path.join("Software", "src")
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp", ".ino")
REGISTRATION_CALL = re.compile(r"\battachInterrupt(?:Arg)?\s*\(")
REGISTRATION_SITES = {
    os.path.join("Software", "src", "lib", "mcp2515_lite", "mcp2515_lite.cpp"): "mcp2515",
    os.path.join("Software", "src", "lib", "pierremolinaro-ACAN2517FD",
                 "ACAN2517FD.cpp"): "fd",
}


def strip_comments_and_strings(text):
    """C++ source with comments and literal contents blanked out.

    A plain grep for attachInterrupt also finds the places this tree only TALKS
    about it - today just the paragraph in mcp2515_lite.cpp explaining why the
    ISR service is installed where it is, which is in a file that registers one
    anyway. So stripping is not what makes the current answer right; it is what
    stops the first prose mention in a third file from failing a build for
    saying the word.
    """
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            out.append(" ")
            i = n if j < 0 else j + 2
        elif c in "\"'":
            quote = c
            out.append(" ")
            i += 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
            i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def registration_census(root):
    """Every file under the source tree that CALLS attachInterrupt."""
    tree = os.path.join(root, SOURCE_TREE)
    if not os.path.isdir(tree):
        raise AuditDidNotRun(
            "no %s under %s, so the registration sites cannot be counted - pass "
            "--source-root pointing at the repository root." % (SOURCE_TREE, root))
    found = set()
    for dirpath, _, filenames in os.walk(tree):
        for filename in filenames:
            if not filename.endswith(SOURCE_SUFFIXES):
                continue
            path = os.path.join(dirpath, filename)
            try:
                with open(path, errors="replace") as fh:
                    text = fh.read()
            except OSError as exc:
                raise AuditDidNotRun("cannot read %s: %s" % (path, exc))
            if REGISTRATION_CALL.search(strip_comments_and_strings(text)):
                found.add(os.path.relpath(path, root))
    return found


def check_registration_sites(root):
    """Raise unless the tree registers interrupts exactly where the presets say."""
    found = registration_census(root)
    expected = set(REGISTRATION_SITES)
    added, gone = sorted(found - expected), sorted(expected - found)
    if not added and not gone:
        print("registration census: %d attachInterrupt site(s), all described by a preset" %
              len(found))
        return
    lines = []
    for path in added:
        lines.append("  NEW      %s - nothing walks its handler; add a preset for it, or "
                     "record here why it needs none" % path)
    for path in gone:
        lines.append("  GONE     %s - was the %s preset's registration; if the interrupt is "
                     "really gone, drop the preset with it" % (path, REGISTRATION_SITES[path]))
    raise AuditDidNotRun(
        "the tree no longer registers GPIO interrupts where this audit's presets say it "
        "does, so a handler may be running through flash windows unwalked:\n%s" %
        "\n".join(lines))


class AuditDidNotRun(Exception):
    """Anything that makes a verdict impossible rather than negative."""


def toolchain(target):
    """The `<dir>/xtensa-<target>-elf-` stem tool names are concatenated onto.

    Resolution mirrors the build's: PLATFORMIO_CORE_DIR when it is set,
    $HOME/.platformio when it is not. It deliberately does NOT chain - a core
    dir that is set but carries no toolchain fails, because a stale
    ~/.platformio answering for the real one is exactly the hazard. HOME is
    read out of the environment mapping rather than via expanduser(), which
    consults the real process environment and would ignore an injected one.
    """
    if target.startswith(RISCV_TARGET_PREFIXES):
        # Said HERE rather than through a missing-file error. The chip is read
        # from the map before any config is looked at, so on a riscv env the
        # first thing that fails is the search for an `xtensa-esp32c3-elf-`
        # toolchain - a message about an install that would never help. The
        # walk matches xtensa call mnemonics and the .iram0.text / .flash.text
        # layout; what it needs is porting, not a different prefix.
        raise AuditDidNotRun(
            "%s is a riscv target and this walker decodes xtensa only - port the "
            "instruction matching and the section layout before carrying %s to it, "
            "rather than pointing it at another toolchain." % (target, FLAG))
    explicit = os.environ.get("PLATFORMIO_CORE_DIR")
    root = explicit or os.path.join(os.environ.get("HOME") or os.path.expanduser("~"), ".platformio")
    prefix = "xtensa-%s-elf-" % target
    bindir = os.path.join(root, TOOLCHAIN_PACKAGE)
    stem = os.path.join(bindir, prefix)
    absent = [t for t in REQUIRED_TOOLS if not os.access(stem + t, os.X_OK)]
    if absent:
        raise AuditDidNotRun(
            "no %s toolchain under %s\n"
            "  core dir: %s (from %s)\n"
            "  missing:  %s\n"
            "Install the toolchain there, or point PLATFORMIO_CORE_DIR at the core "
            "dir your builds use." % (
                prefix, bindir, root,
                "PLATFORMIO_CORE_DIR" if explicit else "the $HOME/.platformio default",
                ", ".join(prefix + t for t in absent)))
    return stem


def repo_sets_flag(root):
    """Whether this repository's TRACKED custom sdkconfig asks for the flag.

    None when the file is not there at all - the tool is then being pointed at
    an image from somewhere else and has nothing to compare against.

    This exists because the per-env GENERATED sdkconfig, which is the file the
    --sdkconfig cross-check reads, is written only by a build that recompiled
    the framework libraries. So it is present on exactly the builds where
    nothing went wrong with those libraries, and absent on every cache hit -
    which means the cross-check is armed when it is least needed and disarmed
    when it is most needed. The tracked file has the opposite property: it is
    always there, and it is the request. Comparing the request against the image
    is what turns "this image does not have the flag" from a reason to skip into
    a reason to stop.
    """
    path = os.path.join(root, CUSTOM_SDKCONFIG)
    if not os.path.exists(path):
        return None
    with open(path, errors="replace") as fh:
        return re.search(r"^%s=y$" % FLAG, fh.read(), re.M) is not None


def sdkconfig_flag(path):
    """Whether the generated sdkconfig at `path` sets the flag.

    Only ever used to CROSS-CHECK the image; the image is what decides. A
    generated config also carries CONFIG_IDF_TARGET_ARCH, and a non-xtensa one
    is refused outright: the walk matches call4/callx4/l32r mnemonics and the
    .iram0.text / .flash.text layout, so a riscv target needs a ported walker
    rather than a different toolchain prefix, and saying so beats emitting a
    "clean" that means "decoded nothing it understood".
    """
    try:
        with open(path) as fh:
            text = fh.read()
    except OSError as exc:
        raise AuditDidNotRun("cannot read the generated sdkconfig %s: %s" % (path, exc))
    # not-set keys are written as a comment, so match the assignment exactly
    flag_set = re.search(r"^%s=y$" % FLAG, text, re.M) is not None
    arch = re.search(r'^CONFIG_IDF_TARGET_ARCH="([^"]+)"$', text, re.M)
    if not arch:
        raise AuditDidNotRun(
            "no CONFIG_IDF_TARGET_ARCH in %s - not a generated sdkconfig?" % path)
    if flag_set and arch.group(1) != "xtensa":
        raise AuditDidNotRun(
            "%s is %s, and this walker decodes xtensa only - port it before carrying "
            "%s to this target." % (path, arch.group(1), FLAG))
    return flag_set


def target_from_map(elf):
    """The chip this image was linked for, per the linker's own map file.

    The map lists every framework-libs archive it pulled in, and those live
    under a per-chip directory. It is written by the same link that produced
    the ELF, so unlike any config file at the project root it cannot describe a
    different env than the image beside it.
    """
    mapfile = os.path.splitext(elf)[0] + ".map"
    if not os.path.exists(mapfile):
        raise AuditDidNotRun(
            "no %s beside the image, so the chip it was linked for is unknown - pass "
            "--target, and note that the wrong one silently mis-decodes the "
            "disassembly." % mapfile)
    chips = set()
    with open(mapfile, errors="replace") as fh:
        for line in fh:
            for m in re.finditer(r"framework-arduinoespressif32-libs/(esp32[a-z0-9]*)/", line):
                chips.add(m.group(1))
    if len(chips) != 1:
        raise AuditDidNotRun(
            "%s names %s framework-libs chip director%s (%s) - cannot tell which "
            "toolchain decodes this image; pass --target." % (
                mapfile, len(chips) or "no", "y" if len(chips) == 1 else "ies",
                ", ".join(sorted(chips)) or "none"))
    return chips.pop()


def sections(tc, elf):
    sec = {}
    out = subprocess.run([tc + "objdump", "-h", elf], capture_output=True, text=True)
    for line in out.stdout.splitlines():
        m = re.match(r"\s*\d+\s+(\S+)\s+([0-9a-f]{8})\s+([0-9a-f]{8})", line)
        if m:
            sec[m.group(1)] = (int(m.group(3), 16), int(m.group(2), 16))
    for want in (".flash.text", ".iram0.text"):
        if want not in sec:
            raise AuditDidNotRun("%s has no %s section (objdump said: %s)" % (
                elf, want, (out.stderr or "nothing").strip()))
    return sec


def symbols(tc, elf):
    syms, by_name = {}, {}
    for line in subprocess.run([tc + "nm", "-C", "-S", elf],
                               capture_output=True, text=True).stdout.splitlines():
        m = re.match(r"^([0-9a-f]{8}) ([0-9a-f]{8}) \S (.+)$", line)
        if m:
            a, s, n = int(m.group(1), 16), int(m.group(2), 16), m.group(3)
            syms[a] = (s, n)
            by_name.setdefault(n, (a, s))
    if not by_name:
        raise AuditDidNotRun("%s yielded no sized symbols - stripped image?" % elf)
    return syms, by_name


# Every xtensa instruction that can move control somewhere else. The windowed
# ABI has FOUR call widths, not two: gcc picks the one that fits the callee's
# register need, and this toolchain emits call12 as readily as call8. Matching
# `call\d` rather than `call\d+` therefore reads `call12` as no call at all -
# its target is neither followed nor reported, which is a hole in exactly the
# direction that makes a gate say "clean".
CALL_RE = re.compile(r"\b(call\d+|callx\d+|j|jx)\b")
# Every hex word the instruction references, however it is printed: the
# instruction's own address, a direct target, and - because this binutils
# annotates l32r with the word it loads - a literal that happens to be a code
# pointer.
HEXWORD_RE = re.compile(r"\b([0-9a-f]{8})\b")


# A disassembly line, whatever the bytes column holds: objdump prints the
# address, the encoded bytes and the mnemonic, and the tests hand in the same
# shape with a stand-in bytes column.
LINE_RE = re.compile(r"^\s*([0-9a-f]{4,16}):\s*\S+\s+(\S+)\s*(.*)$")
# Control transfers whose target is a real instruction boundary INSIDE the
# function. The `.n` suffix is not optional cosmetics: xtensa's narrow forms
# (`bnez.n`, `beqz.n`) are how gcc writes most short branches here, and a
# pattern that misses them records no target, so the descent stops at the first
# `j` and walks a fraction of the function - which is a hole in the direction
# that makes this gate say "clean". Deliberately not l32r: binutils annotates l32r with the literal it
# loads, and a literal that happens to point into this function is data, not an
# instruction - decoding from it would invent code exactly the way the linear
# sweep does.
BRANCH_MNEM_RE = re.compile(r"^(j|jx|b[a-z]+[0-9]*|call\d+|callx\d+|loop[a-z]*)(\.n)?$")
# After one of these, the next byte is only an instruction if something jumps
# there. gcc pads to alignment after them, and padding decodes as nonsense.
# `ill` is the trap gcc plants after a noreturn call or an unreachable branch,
# and the rf* forms return from an exception or interrupt: nothing falls
# through any of them either. Measured over the twelve archived lane images
# the only `ill` on a walked path (panic_abort) is followed by a branch target,
# so this closes a gap the corpus has not yet exercised, not a live finding.
TERMINATORS = frozenset(("ret", "ret.n", "retw", "retw.n", "j", "jx",
                         "ill", "ill.n", "rfe", "rfde", "rfi", "rfwo", "rfwu", "rfue"))


def trusted_lines(disassemble, addr, size, entry_addrs=()):
    """One function's disassembly, decoded from instruction boundaries only.

    THE DEFECT THIS CLOSES. objdump -d decodes a range as one linear sweep, and
    xtensa instructions are 2 and 3 bytes: the first byte after a `ret` or a `j`
    is usually alignment padding, so the sweep resumes half an instruction out
    and every byte after it decodes as garbage until the stream happens to
    resynchronise. Garbage disassembles into plausible instructions - on
    `stark_330` it produced a `call4` into a flash address that no code reaches,
    and the audit reported an IRAM function calling GeelySeaBattery::readDiagData().
    The verdict then depended on unrelated flash layout, so a lane's CI went red
    at random.

    So decode the way the processor reaches the bytes: start at the entry, stop
    at each unconditional terminator, and resume only where something branches -
    a recursive descent. A target discovered mid-sweep is re-decoded FROM that
    address, because the missed instructions are not in the linear text at all;
    on stark the real boundary 0x4008b838 has no line in the sweep that begins
    at 0x4008b836.

    Symbol entries inside the range are boundaries too: a function whose symbol
    covers several entry points still has code at each.
    """
    end = addr + size
    decoded = {}
    queued = {addr} | {a for a in entry_addrs if addr < a < end}
    work = sorted(queued)
    while work:
        start = work.pop(0)
        if start in decoded:
            continue
        for line in disassemble(start, end - start).splitlines():
            m = LINE_RE.match(line)
            if not m:
                continue
            at = int(m.group(1), 16)
            if at < start or at >= end:
                continue
            if at in decoded:
                break  # this path has run into one already decoded
            decoded[at] = line
            mnemonic = m.group(2)
            if BRANCH_MNEM_RE.match(mnemonic):
                for tok in HEXWORD_RE.findall(m.group(3)):
                    target = int(tok, 16)
                    if addr <= target < end and target not in queued:
                        queued.add(target)
                        work.append(target)
            if mnemonic in TERMINATORS:
                break
        work.sort()
    return [decoded[a] for a in sorted(decoded)]


def line_targets(line, in_flash, in_iram, entry):
    """(flash targets, iram targets) one disassembly line reaches.

    Pure, so the classification this whole audit rests on can be driven from a
    test with synthetic objdump text rather than only end-to-end against an
    image. Every defect found in it so far was invisible end-to-end: a call
    width the audited path happens not to use is a hole that no amount of
    mutating real firmware reveals.

    `entry` is the set of symbol addresses. objdump decodes each function's
    trailing literal pool as instructions too, and a data word can disassemble
    into a plausible-looking call into flash - so only a target that is exactly
    a function ENTRY counts; anything landing mid-symbol is pool garbage.
    """
    is_call = CALL_RE.search(line) is not None
    is_literal = "l32r" in line
    flash, iram = [], []
    for tok in HEXWORD_RE.findall(line):
        t = int(tok, 16)
        if t not in entry:
            continue
        if in_flash(t) and (is_call or is_literal):
            flash.append(t)
        elif in_iram(t) and is_call:
            iram.append(t)
    return flash, iram


def objdump_disassembler(tc, elf):
    """The real disassembler: one function's address range, decoded."""
    def disassemble(addr, size):
        return subprocess.run([tc + "objdump", "-d", "-C", "--start-address=%d" % addr,
                               "--stop-address=%d" % (addr + size), elf],
                              capture_output=True, text=True).stdout
    return disassemble


def walk(disassemble, preset, syms, by_name, in_flash, in_iram):
    """(walked, flash-resident targets, roots that inlined away) for one preset."""
    roots = PRESETS[preset]
    absent = [r for r in roots["taken"] if r not in by_name]
    if absent:
        raise AuditDidNotRun(
            "preset %r: root(s) absent from the image, so the interrupt path is not what "
            "this audit describes - update the preset or the registration:\n%s" % (
                preset, "\n".join("  " + r for r in absent)))
    inlined = [r for r in roots["may_inline"] if r not in by_name]

    entry = set(syms)

    def name_of(addr):
        for a, (s, n) in syms.items():
            if a <= addr < a + max(s, 1):
                return n if addr == a else "%s+%#x" % (n, addr - a)
        return "?"

    queue = [r for r in roots["taken"] + roots["may_inline"] if r in by_name]
    seen, bad = set(), []
    while queue:
        name = queue.pop(0)
        if name in seen:
            continue
        seen.add(name)
        addr, size = by_name[name]
        # A function on this path that is ITSELF in flash is the finding, not
        # only its callees. The roots are meant to be safe by construction -
        # IRAM_ATTR is a section attribute - but that is a property of the
        # SOURCE, and the whole reason this tool reads the image is that source
        # properties are not what ships: the attribute sits behind an #ifdef in
        # the vendored FD driver, and a callee reached through an IRAM caller
        # has no attribute at all. Checking costs one comparison.
        if in_flash(addr):
            bad.append((name, name, addr))
            continue
        for line in trusted_lines(disassemble, addr, size, entry):
            flash_targets, iram_targets = line_targets(line, in_flash, in_iram, entry)
            for t in flash_targets:
                bad.append((name, name_of(t), t))
            for t in iram_targets:
                n = name_of(t)
                if n in by_name and n not in seen:
                    queue.append(n)
    return seen, sorted(set(bad)), inlined

def main(argv):
    ap = argparse.ArgumentParser(description="ISR IRAM residency audit of a linked image")
    ap.add_argument("elf", help="path to firmware.elf")
    ap.add_argument("presets", nargs="*",
                    help="interrupt roots to walk (default: all of %s)" % ", ".join(sorted(PRESETS)))
    ap.add_argument("--sdkconfig", metavar="PATH",
                    help="generated per-env sdkconfig, cross-checked against the image when "
                         "present; the image is what decides")
    ap.add_argument("--target", metavar="IDF_TARGET",
                    help="override the chip read from firmware.map (e.g. esp32, esp32s3)")
    ap.add_argument("--source-root", metavar="PATH", default=REPO_ROOT,
                    help="repository root whose %s is counted for attachInterrupt "
                         "registration sites (default: the tree this script lives in)" %
                         SOURCE_TREE)
    args = ap.parse_args(argv)

    presets = args.presets or sorted(PRESETS)
    unknown = [p for p in presets if p not in PRESETS]
    if unknown:
        ap.error("unknown preset(s) %s - one of %s" % (", ".join(unknown), ", ".join(sorted(PRESETS))))

    if not os.path.exists(args.elf):
        raise AuditDidNotRun("no image at %s - did the build run?" % args.elf)

    target = args.target or target_from_map(args.elf)
    tc = toolchain(target)
    sec = sections(tc, args.elf)
    flash_lo, flash_sz = sec[".flash.text"]
    iram_lo, iram_sz = sec[".iram0.text"]
    flash_hi, iram_hi = flash_lo + flash_sz, iram_lo + iram_sz
    in_flash = lambda a: flash_lo <= a < flash_hi  # noqa: E731
    in_iram = lambda a: iram_lo <= a < iram_hi  # noqa: E731
    syms, by_name = symbols(tc, args.elf)
    disassemble = objdump_disassembler(tc, args.elf)

    # The gate: the flag's whole effect is that this function is resident, so the
    # image answers "is the flag on" without a config file to go stale.
    if DISPATCHER not in by_name:
        raise AuditDidNotRun(
            "no %s in %s - the Arduino GPIO dispatcher is what %s makes resident, so "
            "without it there is no way to tell whether the flag is in effect." % (
                DISPATCHER, args.elf, FLAG))
    dispatcher_addr = by_name[DISPATCHER][0]
    flag_in_effect = in_iram(dispatcher_addr)
    if not flag_in_effect and not in_flash(dispatcher_addr):
        raise AuditDidNotRun(
            "%s is at %#x, in neither .iram0.text nor .flash.text - this image is not "
            "laid out the way the audit assumes." % (DISPATCHER, dispatcher_addr))

    if args.sdkconfig:
        # Free consistency check, and it has teeth: a config left behind by a
        # sibling env describes a different image than the one being audited.
        configured = sdkconfig_flag(args.sdkconfig)
        if configured != flag_in_effect:
            raise AuditDidNotRun(
                "%s says %s is %s, but %s in this image is %s-resident - the config does "
                "not describe this build." % (
                    args.sdkconfig, FLAG, "y" if configured else "not set", DISPATCHER,
                    "IRAM" if flag_in_effect else "flash"))
        print("%s agrees with the image (%s %s)" % (
            args.sdkconfig, FLAG, "=y" if configured else "not set"))

    print("target %s   .iram0.text = %#x..%#x   .flash.text = %#x..%#x" % (
        target, iram_lo, iram_hi, flash_lo, flash_hi))
    if not flag_in_effect:
        # "The flag is off" and "the flag was asked for and did not arrive" look
        # identical in the image, and only one of them is a reason to skip. The
        # tracked config says which. This is not hypothetical: building four envs
        # in one hold on a machine where another tree had recompiled the shared
        # framework libraries without the flag produced exactly this image, and
        # without the comparison below the audit called it "nothing to audit"
        # and exited 0 - silently passing the regression the flag exists to
        # prevent.
        if repo_sets_flag(args.source_root):
            raise AuditDidNotRun(
                "%s sets %s=y, but %s is at %#x, inside .flash.text - this image did not "
                "get the configuration this repository asks for, so it carries the masked-"
                "interrupt behaviour the flag exists to remove. Nothing here is unsafe to "
                "RUN; the build is wrong. The usual cause is framework libraries left by a "
                "build that did not carry the flag: recompile them from this config, or "
                "pass --source-root if this image is not from this tree." % (
                    CUSTOM_SDKCONFIG, FLAG, DISPATCHER, dispatcher_addr))
        print("%s is at %#x, in .flash.text: %s is not in effect in this image, so the "
              "GPIO dispatcher is masked through flash windows rather than run through "
              "them, and the residency condition does not apply. Nothing to audit." % (
                  DISPATCHER, dispatcher_addr, FLAG))
        return 0

    # Before walking anything: is the set of interrupts this audit describes still
    # the set the tree registers? A preset whose root vanished is caught by the
    # walk; a registration nobody wrote a preset for is caught only here.
    check_registration_sites(args.source_root)

    findings = {}
    for preset in presets:
        seen, bad, inlined = walk(disassemble, preset, syms, by_name, in_flash, in_iram)
        print("%-11s walked %d IRAM function(s) reachable from its interrupt roots - %s" % (
            preset, len(seen), "%d flash-resident target(s)" % len(bad) if bad else "clean"))
        for r in inlined:
            # Covered anyway: it was inlined into a caller that IS walked.
            print("%-11s   inlined away (no out-of-line symbol): %s" % (preset, r))
        if bad:
            findings[preset] = bad

    if findings:
        print("\nCODE IN FLASH REACHED FROM THE INTERRUPT (fetch with the cache off = crash):")
        for preset, bad in findings.items():
            for src, dst, addr in bad:
                print("  [%s] %s\n      -> %08x  %s" % (preset, src, addr, dst))
        print("\nEither mark the callee IRAM_ATTR, or take it off the interrupt path. If it is a "
              "header function the compiler emitted out-of-line, the attribute belongs on the "
              "definition it was emitted from.")
        return 1
    print("clean: nothing these interrupts reach lives in .flash.text")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except AuditDidNotRun as exc:
        print("%s: %s" % (os.path.basename(__file__), exc), file=sys.stderr)
        sys.exit(2)
