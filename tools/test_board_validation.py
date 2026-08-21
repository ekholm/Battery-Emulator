#!/usr/bin/env python3
"""Prove board_gen.py rejects declarations it should reject.

A validator is only worth its runtime if it fails on bad input, and the way to
be sure is to feed it bad input rather than to read it. Each case mutates a
real declaration into something the hardware could not support and requires a
nonzero exit whose message names the board, the feature and the field - and
requires that no header was touched.

The accepted cases pin the requirement shapes that exist in the drivers, so a
later simplification that flattened one of them fails here: the CAN-FD
interrupt is INT *or* INT0+INT1; a pin that is chosen at runtime still counts
as present; a pin declared NC does not.

Usage: python3 tools/test_board_validation.py
"""
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).parent.parent
BOARDS = ROOT / 'Software' / 'boards'
HEADERS = ROOT / 'Software' / 'src' / 'devboard' / 'hal'
GEN = ROOT / 'tools' / 'board_gen.py'

sys.path.insert(0, str(ROOT / 'tools'))
import board_gen  # noqa: E402  - after ROOT, and the declarations have one parser

failures = []


def run(board, text):
    """Run the generator over one declaration in a sandbox; return
    (rc, output, whether any header changed, the headers as generated).

    The generated text is returned rather than read back from the tree: a
    check that opens the real header proves nothing about this run, because
    the file still holds whatever the last real generator run left there."""
    with tempfile.TemporaryDirectory() as tmp:
        boards, headers = Path(tmp) / 'boards', Path(tmp) / 'hal'
        boards.mkdir()
        headers.mkdir()
        # The generator resolves add-ons next to the boards directory it was
        # given, so the sandbox needs them too: without this every board naming
        # an add-on would be checked against an empty set, and the add-on cases
        # would pass without exercising anything.
        addons = Path(tmp) / 'addons'
        addons.mkdir()
        for src in (ROOT / 'Software' / 'addons').glob('*.yaml'):
            shutil.copy(src, addons / src.name)
        for src in HEADERS.glob('hw_*.h'):
            shutil.copy(src, headers / src.name)
        before = {p.name: p.read_text(encoding='utf-8') for p in headers.iterdir()}
        (boards / f'{board}.yaml').write_text(text, encoding='utf-8')
        proc = subprocess.run(
            [sys.executable, str(GEN), '--boards', str(boards), '--headers', str(headers)],
            capture_output=True, text=True)
        after = {p.name: p.read_text(encoding='utf-8') for p in headers.iterdir()}
        # capabilities.h lands in the same directory but is not a board header;
        # excluded so that "did a header change" keeps meaning what it says.
        touched = {k: v for k, v in after.items() if k != 'capabilities.h'}
        return proc.returncode, proc.stdout + proc.stderr, before != touched, after


def expect_reject(case, text, *must_mention, board='stark'):
    rc, output, changed, _ = run(board, text)
    if rc == 0:
        failures.append(f'{case}: accepted a declaration it must reject')
        return
    missing = [m for m in must_mention if m not in output]
    if missing:
        failures.append(f'{case}: rejected, but the message never mentions {missing}\n'
                        f'    {output.strip()}')
    if changed:
        failures.append(f'{case}: rejected but still rewrote a header')


def expect_accept(case, text, board='stark'):
    rc, output, _, _ = run(board, text)
    if rc != 0:
        failures.append(f'{case}: rejected a valid declaration\n    {output.strip()}')


def drop(text, pattern):
    """Remove a key: value pair from an inline map."""
    new = re.sub(pattern, '', text, count=1)
    if new == text:
        raise AssertionError(f'test setup: nothing matched {pattern!r}')
    return new


def main():
    stark = (BOARDS / 'stark.yaml').read_text(encoding='utf-8')
    lilygo = (BOARDS / 'lilygo.yaml').read_text(encoding='utf-8')

    # If the committed declarations did not pass, every rejection below would
    # be meaningless - it would just be failing for an unrelated reason.
    expect_accept('committed stark', stark)
    expect_accept('committed lilygo', lilygo, board='lilygo')

    # A feature missing a pin its driver reads.
    expect_reject('missing required field', drop(stark, r'positive: 32, '),
                  'stark', 'contactors', 'positive')

    # NC means the board deliberately lacks the pin, so it must not satisfy a
    # requirement even though the field is present. This is the subtle one.
    expect_reject('required field declared NC', stark.replace('positive: 32', 'positive: NC'),
                  'stark', 'contactors', 'positive')

    # A pin chosen at runtime is still a real role: the getter is hand-written,
    # but the hardware is there, so the feature must validate.
    expect_accept('late-bound pin satisfies a requirement',
                  stark.replace('positive: 32', 'positive: setting'))

    # CAN-FD interrupt: INT, or INT0+INT1, and half of the pair is not enough.
    expect_reject('canfd with no interrupt', drop(stark, r', int: 35'),
                  'stark', 'can', 'mcp2518fd', 'int')
    expect_accept('canfd with INT0+INT1 instead of INT',
                  drop(stark, r', int: 35').replace('cs: 18', 'cs: 18, int0: 36, int1: 39'))
    expect_reject('canfd with only half of INT0/INT1',
                  drop(stark, r', int: 35').replace('cs: 18', 'cs: 18, int0: 36'),
                  'stark', 'can', 'int')

    # Buses are declared once and referenced; a dangling or incomplete bus is
    # exactly the kind of thing the old flat pin map could not express.
    expect_reject('reference to an undeclared bus', stark.replace('bus: SPI1', 'bus: SPI9'),
                  'stark', 'SPI9')
    expect_reject('bus missing a pin', drop(stark, r'clk: variant, '),
                  'stark', 'SPI1', 'clk')

    # Typos must not pass silently.
    expect_reject('unknown driver', stark.replace('driver: native', 'driver: nativ'),
                  'stark', 'nativ')
    expect_reject('unknown field', stark.replace('cs: 12, int: 14', 'cs: 12, int: 14, csx: 9'),
                  'stark', 'csx')
    # The ceiling is four CAN-FD instances, because that is how far the getter
    # names go (MCP2517_CS..CS4). A fifth is checked further down, where the
    # four-instance case that made room for it is set up.

    # sd_spi: the SPI-attached card. Its chip select is not optional the way
    # SD_MMC's is - an SPI slave with no CS is not addressable - and a board
    # has one kind of card interface, never both.
    dfrobot = (BOARDS / 'dfrobot_edge101.yaml').read_text(encoding='utf-8')
    expect_accept('committed dfrobot_edge101', dfrobot, board='dfrobot_edge101')
    expect_reject('sd_spi without chip select', drop(dfrobot, r', cs: 5'),
                  'dfrobot_edge101', 'sd_spi', 'cs', board='dfrobot_edge101')
    expect_reject('both card interfaces on one board',
                  dfrobot.replace('sd_spi:', 'sd_mmc:\n  - {miso: 39, mosi: 12, clk: 14}\nsd_spi:'),
                  'dfrobot_edge101', 'sd_mmc', 'sd_spi', board='dfrobot_edge101')

    # The guard is what makes sd_spi expressible at all: the group is compiled
    # out unless SDCARD is defined, so the block has to carry the #ifdef. A
    # generator that emitted the getters bare would still pass every other
    # check here and silently change what the board compiles.
    rc, output, _, generated = run('dfrobot_edge101', dfrobot)
    if rc != 0:
        failures.append(f'sd_spi emission: the generator failed\n    {output.strip()}')
    else:
        header = generated['hw_dfrobot_edge101.h']
        for marker in ('#ifdef SDCARD', '#endif  // SDCARD', 'uint8_t SD_SPI_BUS() override'):
            if marker not in header:
                failures.append(f'sd_spi emission: the generated header lacks {marker!r}')

    # Ethernet: the management pair is what makes the PHY reachable, so it is
    # required; the driver name is a chip, so a typo must not pass; and its
    # pins are dedicated functions, so a collision with another dedicated
    # feature (here the SD chip select) must be reported. The committed
    # declaration accepting (above) pins the valid shape.
    expect_reject('ethernet without mdio', drop(dfrobot, r'mdio: 13, '),
                  'dfrobot_edge101', 'ethernet', 'mdio', board='dfrobot_edge101')
    expect_reject('unknown ethernet driver', dfrobot.replace('driver: ip101', 'driver: ip102'),
                  'dfrobot_edge101', 'ip102', board='dfrobot_edge101')
    expect_reject('ethernet pin colliding with the SD chip select',
                  dfrobot.replace('mdc: 4', 'mdc: 5'),
                  'dfrobot_edge101', 'GPIO 5', 'ethernet.mdc', 'sd_spi.cs', board='dfrobot_edge101')
    # The alternative-fitment carve-out exempts DIFFERENT drivers of
    # one feature sharing a header - one driver claiming a GPIO through two of
    # its own roles is still a declaration mistake, and this is the one shape
    # of the rule no committed board anchors (the LilyGo anchors the exemption,
    # not its boundary).
    expect_reject('one driver claiming a pin with two roles',
                  lilygo.replace('driver: mcp2515, bus: SPI1, cs: 18, int: 35',
                                 'driver: mcp2515, bus: SPI1, cs: 18, int: 18'),
                  'lilygo', 'GPIO 18', 'can.cs', 'can.int', board='lilygo')

    rc, output, _, generated = run('dfrobot_edge101', dfrobot)
    if rc != 0:
        failures.append(f'ethernet emission: the generator failed\n    {output.strip()}')
    else:
        header = generated['hw_dfrobot_edge101.h']
        for marker in ('ETH_MDC_PIN() { return GPIO_NUM_4; }', 'ETH_MDIO_PIN() { return GPIO_NUM_13; }',
                       'ETH_POWER_PIN() { return GPIO_NUM_2; }', 'uint8_t ETH_PHY_ADDR() { return 1; }',
                       'uint8_t ETH_CLK_MODE() { return 0; }'):
            if marker not in header:
                failures.append(f'ethernet emission: the generated header lacks {marker!r}')

    # Four CAN-FD controllers on one bus. The Stark's isolated dual-FD add-on
    # board takes the count past the two the schema used to allow, and the
    # third and fourth share the first instance's bus and crystal rather than
    # declaring their own - so this pins BOTH that they emit their own CS/INT
    # and that they emit no second set of bus getters.
    four_fd = stark.replace(
        "  - {driver: mcp2518fd, bus: SPI1, cs: 12, int: 14}",
        "  - {driver: mcp2518fd, bus: SPI1, cs: 12, int: 14}\n"
        "  - {driver: mcp2518fd, bus: SPI1, cs: 13, int: 15}\n"
        "  - {driver: mcp2518fd, bus: SPI1, cs: 19, int: 16}")
    rc, output, _, generated = run('stark', four_fd)
    if rc != 0:
        failures.append(f'four CAN-FD instances: rejected a valid declaration\n    {output.strip()}')
    else:
        header = generated['hw_stark.h']
        for marker in ('gpio_num_t MCP2517_CS3() { return GPIO_NUM_13; }',
                       'gpio_num_t MCP2517_INT3() { return GPIO_NUM_15; }',
                       'gpio_num_t MCP2517_CS4() { return GPIO_NUM_19; }',
                       'gpio_num_t MCP2517_INT4() { return GPIO_NUM_16; }'):
            if marker not in header:
                failures.append(f'four CAN-FD instances: the generated header lacks {marker!r}')
        if 'MCP2517_SCK3' in header or 'MCP2517_SCK4' in header:
            failures.append('four CAN-FD instances: instances 3/4 emitted bus getters of their own, '
                            'but they share the first instance\'s bus')

    # A fifth is not a thing the getter names can express, and must be refused
    # rather than silently dropped.
    expect_reject('a fifth CAN-FD instance',
                  four_fd.replace("  - {driver: mcp2518fd, bus: SPI1, cs: 19, int: 16}",
                                  "  - {driver: mcp2518fd, bus: SPI1, cs: 19, int: 16}\n"
                                  "  - {driver: mcp2518fd, bus: SPI1, cs: 20, int: 21}"),
                  'stark', 'more instances declared than the driver supports')

    # --- add-ons: the board says which pins it wires, the template says which
    # signals the module needs and which way each one goes. Neither half can
    # check itself, so these drive the seam between them.
    lilygo = (BOARDS / 'lilygo.yaml').read_text(encoding='utf-8')

    expect_reject('an add-on with no definition',
                  lilygo.replace('addon: mcp2515', 'addon: mcp2515_deluxe'),
                  'lilygo', 'no definition in Software/addons', board='lilygo')

    # A signal the module RECEIVES has to be driven, so binding it to a pin
    # that can only be read cannot work. Nothing catches this today: the pin
    # simply never asserts and the module never answers.
    expect_reject('an add-on input on a read-only pin',
                  lilygo.replace('cs: 18, int: 35, addon: mcp2515',
                                 'cs: 34, int: 35, addon: mcp2515'),
                  'lilygo', 'input-only on the esp32', board='lilygo')

    # ...and that check is chip-specific, so it must not fire on a part where
    # the same number is a perfectly good output.
    ws = (BOARDS / 'waveshare.yaml').read_text(encoding='utf-8')
    expect_accept('an S3 board driving GPIO 34',
                  ws.replace('cs: 13, int: 14', 'cs: 34, int: 14, addon: mcp2518fd'),
                  board='waveshare')

    # A module that needs a bus must be on one.
    expect_reject('an add-on needing a bus with none referenced',
                  lilygo.replace('  - {driver: mcp2515, bus: SPI1, cs: 18, int: 35, addon: mcp2515}',
                                 '  - {driver: mcp2515, cs: 18, int: 35, addon: mcp2515}'),
                  'lilygo', 'needs a spi bus', board='lilygo')

    # The isolated dual-FD card is the add-on this mechanism was built for, and
    # until here no test ever BOUND it - it only had to parse. A four-instance
    # Stark naming it on the two add-on channels must validate cleanly, and its
    # chip select is a signal the card receives, so the direction check has to
    # know the template by name when the pin cannot be driven.
    dual_fd = four_fd.replace(
        "  - {driver: mcp2518fd, bus: SPI1, cs: 13, int: 15}\n"
        "  - {driver: mcp2518fd, bus: SPI1, cs: 19, int: 16}",
        "  - {driver: mcp2518fd, bus: SPI1, cs: 13, int: 15, addon: dual_isolated_canfd}\n"
        "  - {driver: mcp2518fd, bus: SPI1, cs: 19, int: 16, addon: dual_isolated_canfd}")
    expect_accept('the isolated dual-FD card on instances three and four', dual_fd)
    expect_reject('the dual-FD card with a chip select it cannot drive',
                  dual_fd.replace('cs: 13, int: 15, addon: dual_isolated_canfd',
                                  'cs: 36, int: 15, addon: dual_isolated_canfd'),
                  'stark', 'dual_isolated_canfd', 'input-only on the esp32')

    # Optional signals are optional. The MCP2515's reset can be tied high on
    # the module, and three of the four boards carrying one do exactly that -
    # if this ever becomes an error the templates have drifted from the tree.
    expect_accept('an add-on with its optional signal unwired', lilygo, board='lilygo')

    # But a mandatory one is not.
    expect_reject('an add-on missing a signal it must have',
                  lilygo.replace('cs: 18, int: 35, addon: mcp2518fd', 'int: 35, addon: mcp2518fd'),
                  'lilygo', 'needs "cs" driven', board='lilygo')

    # Every template on disk has to parse and name itself, or a board naming
    # one gets "no definition" for a reason that has nothing to do with the board.
    try:
        addons = board_gen.load_addons()
    except board_gen.DeclError as exc:
        addons = {}
        failures.append(f'add-on templates: one does not load\n    {exc}')
    for name, decl in sorted(addons.items()):
        if not decl.get('pins'):
            failures.append(f'add-on {name}: declares no pins, so it checks nothing')
        for pin, direction in (decl.get('pins') or {}).items():
            if direction not in ('in', 'out'):
                failures.append(f'add-on {name}: pin "{pin}" has direction "{direction}", '
                                f'which is neither in nor out')
        for pin in (decl.get('optional') or []):
            if pin not in (decl.get('pins') or {}):
                failures.append(f'add-on {name}: "{pin}" is marked optional but is not a pin')
    if 'dual_isolated_canfd' not in addons:
        failures.append('the dual isolated CAN-FD add-on has no template')

    # Two product labels cannot name the same physical output.
    expect_reject('two outputs on one GPIO',
                  stark.replace('{label: "Output 3", gpio: 32', '{label: "Output 3", gpio: 33'),
                  'stark', 'Output 3', '33')

    # The parser refuses what it does not understand rather than guessing.
    expect_reject('unparseable indent', stark.replace('rs485:', 'rs485:\n      deep: 1'),
                  'indent')

    # A board with no build macro, or one that is not a macro, gets no branch in
    # the capability header's #if chain - and would silently have no capability
    # set rather than failing to build.
    expect_reject('no macro', drop(stark, r'macro: HW_STARK\n'), 'stark', 'macro')
    expect_reject('macro is not one', stark.replace('macro: HW_STARK', 'macro: stark'),
                  'stark', 'HW_SOMETHING')

    # THE MIGRATION'S SAFETY ARGUMENT, checked rather than asserted: a row that
    # asks for BoardCap::SdCard must be live on exactly the builds where the
    # `#ifdef SDCARD` it replaces is true. The two are derived from different
    # files - the declarations and platformio.ini - so nothing but this test
    # keeps them in step, and if they ever diverge the migration has silently
    # changed which boards log to a card.
    ini = (ROOT / 'platformio.ini').read_text(encoding='utf-8')
    sdcard_macros = set()
    for env in re.split(r'^\[env:', ini, flags=re.M)[1:]:
        block = env.split('\n[', 1)[0]
        if re.search(r'^\s*-D SDCARD\s*$', block, re.M):
            sdcard_macros.update(re.findall(r'-D (HW_[A-Z0-9_]+)', block))
    declaring = set()
    for path in sorted(BOARDS.glob('*.yaml')):
        data = board_gen.parse_yaml_subset(path.read_text(encoding='utf-8'), path.name)
        if 'SdCard' in board_gen.caps_of(data):
            declaring.add(data[board_gen.MACRO_KEY])
    if declaring != sdcard_macros:
        failures.append('SdCard capability and -D SDCARD disagree: declared by '
                        f'{sorted(declaring)}, built with the define on {sorted(sdcard_macros)}')

    # chip/flash_mb are declared, but platformio.ini also knows both for every
    # board an env builds. Two files stating the same fact drift unless
    # something compares them, and a wrong flash size here would mislead
    # anything reasoning about what a build can hold.
    for path in sorted(BOARDS.glob('*.yaml')):
        data = board_gen.parse_yaml_subset(path.read_text(encoding='utf-8'), path.name)
        macro = data.get(board_gen.MACRO_KEY)
        env = None
        for block in re.split(r'^\[env:', ini, flags=re.M)[1:]:
            body = block.split('\n[', 1)[0]
            if f'-D {macro}' in body:
                env = (block.split(']')[0], body)
                break
        if env is None:
            continue  # no env builds it; the declaration is the only source
        env_name, body = env
        base = ''
        extends = re.search(r'extends\s*=\s*(\S+)', body)
        if extends:
            m = re.search(r'^\[' + extends.group(1) + r'\]([\s\S]*?)(?=^\[)', ini, flags=re.M)
            base = m.group(1) if m else ''
        expect_s3 = 's3' in (extends.group(1) if extends else '')
        if (data.get('chip') == 'esp32s3') != expect_s3:
            failures.append(f'{data["board"]}: declares chip "{data.get("chip")}" but {env_name} '
                            f'builds it as {"an ESP32-S3" if expect_s3 else "a classic ESP32"}')
        size = re.search(r'flash_size\s*=\s*(\d+)MB', body) or re.search(r'flash_size\s*=\s*(\d+)MB', base)
        if size and int(size.group(1)) != int(data.get('flash_mb', 0)):
            failures.append(f'{data["board"]}: declares flash_mb {data.get("flash_mb")} but '
                            f'{env_name} builds it with flash_size {size.group(1)}MB')

    # A board whose declared chip cannot have the pins it declares.
    expect_reject('chip contradicted by its own pins',
                  stark.replace('chip: esp32', 'chip: esp32s3'),
                  'stark', 'only the esp32 has')
    expect_reject('no chip at all', drop(stark, r'chip: esp32\n'), 'stark', 'chip must be one of')
    expect_reject('a nonsense flash size',
                  stark.replace('flash_mb: 8', 'flash_mb: none'), 'stark', 'flash_mb')

    # Ids are preserved across regeneration. A feature added in the middle of
    # the emission order must not renumber the enumerators already emitted -
    # that is the difference between one added line and a rewritten file.
    caps = board_gen.capabilities_header([], '')
    grown = board_gen.capabilities_header([], caps.replace('  Rs485,', '  Zigbee,\n  Rs485,'))
    order = board_gen.existing_cap_order(grown)
    if order[:1] != ['Zigbee']:
        failures.append(f'capability ids are not append-only: {order[:3]} lost its first entry')
    if 'Zigbee' in grown and 'no declaration carries this any more' not in grown:
        failures.append('a capability no declaration carries is kept but not marked as such')

    # A declaration nothing builds is a declaration nothing checks: no CI job
    # compiles the board, and board_gen's chip/flash cross-check has no env to
    # compare against, so those two fields are whatever the file says. The 3LB
    # sat in exactly that state from 2024 until this was added.
    # Commented lines are dropped first: retiring an env by prefixing `;` is
    # the usual soft-disable in an ini file, and a define that only survives
    # inside a comment builds nothing, which the guard used to accept.
    ini = '\n'.join(line for line in (ROOT / 'platformio.ini').read_text(encoding='utf-8').splitlines()
                    if not line.lstrip().startswith(';'))
    for declaration in sorted(BOARDS.glob('*.yaml')):
        macro = re.search(r'^macro:\s*(\S+)', declaration.read_text(encoding='utf-8'), re.M)
        if macro is None:
            failures.append(f'{declaration.name} declares no macro')
            continue
        if not re.search(r'-D\s+%s\b' % re.escape(macro.group(1)), ini):
            failures.append(f'{declaration.name} is declared but no platformio env defines '
                            f'{macro.group(1)} - nothing builds it, so nothing checks it')

    # ...and an env in platformio.ini is not a build either. CI compiles what
    # its matrix lists, so a board whose env never reaches the workflow passes
    # the check above while nothing ever compiles it. The 3LB's matrix row was
    # hand-added in the same commit as its env (item 31), which is precisely
    # the coupling nothing was checking - the next board gets it right only if
    # whoever adds it remembers two files.
    #
    # The matrix is read with a regex rather than a yaml parse, in the style of
    # the rest of this file - and the empty case is a failure, not a pass: a
    # workflow that moved or renamed the key would otherwise clear every board
    # here while compiling none of them.
    workflow = ROOT / '.github' / 'workflows' / 'compile-common-image.yml'
    matrix_envs = set()
    if not workflow.exists():
        failures.append(f'{workflow.name} is missing - CI coverage cannot be checked')
    else:
        matrix_envs = set(re.findall(r'^\s*pio_env:\s*["\']?([A-Za-z0-9_]+)',
                                     workflow.read_text(encoding='utf-8'), re.M))
        if not matrix_envs:
            failures.append(f'{workflow.name} lists no pio_env - the CI coverage check would '
                            'pass every board without compiling any of them')

    if matrix_envs:
        for declaration in sorted(BOARDS.glob('*.yaml')):
            macro = re.search(r'^macro:\s*(\S+)', declaration.read_text(encoding='utf-8'), re.M)
            if macro is None:
                continue  # already reported above
            envs = {block.split(']')[0] for block in re.split(r'^\[env:', ini, flags=re.M)[1:]
                    if re.search(r'-D\s+%s\b' % re.escape(macro.group(1)), block.split('\n[', 1)[0])}
            if envs and not envs & matrix_envs:
                failures.append(f'{declaration.name} is built by {sorted(envs)} but none of those '
                                f'envs is in {workflow.name} - CI never compiles this board')

    if failures:
        print(f'board validation: {len(failures)} FAILED')
        for f in failures:
            print(f'  - {f}')
        sys.exit(1)
    print('board validation: all cases pass (rejections rejected, valid shapes accepted)')


if __name__ == '__main__':
    main()
