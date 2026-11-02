"""
PlatformIO pre-build script: stamp this fork's build marker.

Upstream numbers its releases, but a fork rebuilt several times a day has no way
to tell one binary from the next -- neither the .bin files waiting on the desk
nor the firmware already on the device. This stamps both.

Version format: YYYYMMDDNN -- the build date plus a counter starting at 01 each
day, so 2026080901 is the first build of 9 August 2026.

The counter lives in .ost-build (gitignored) and is deliberately local to this
machine. It answers "which build is on the device", not "which release is this",
so there is nothing for anyone else to reproduce. A fresh clone restarts at 01;
the date in front keeps the ordering right regardless.

Copies the built image to dist/ under a name carrying the number. Diagnostic
builds (INPUT_DIAG) get a -diag suffix so the two cannot be confused on the card.
Images for another board carry its name (-x4pro) before that suffix; the X3 image
from the default env keeps the bare name.

Only the default env advances the counter. Another board's env reuses the number
the default build just took, so the X3 and X4 Pro images of one change share it.

The number stays out of the compile: defining it (a past OST_VERSION macro) put
a value that changes every build into every translation unit's command line,
which invalidated all objects -- libraries included -- and made each no-change
rebuild a 3-minute full recompile. Nothing ever read the macro; the filename is
the only consumer. If the firmware is ever to display it, generate a header and
include it from the one file that shows it.
"""

import datetime
import os
import shlex
import shutil
import sys

COUNTER_FILE = '.ost-build'
DIST_DIR = 'dist'


def warn(msg):
    print(f'WARNING [ost_version.py]: {msg}', file=sys.stderr)


# PlatformIO env -> board tag in the file name. The default env (X3) has none.
BOARD_TAGS = {'x4pro': '-x4pro'}


def next_version(project_dir, advance=True):
    """Return YYYYMMDDNN, advancing the per-day counter unless advance is False."""
    today = datetime.date.today().strftime('%Y%m%d')
    path = os.path.join(project_dir, COUNTER_FILE)

    stored_date, stored_seq = '', 0
    try:
        with open(path, 'r', encoding='utf-8') as f:
            stored_date, _, seq_text = f.read().strip().partition(' ')
            stored_seq = int(seq_text)
    except FileNotFoundError:
        pass
    except (ValueError, OSError) as e:
        warn(f'could not read {COUNTER_FILE} ({e}); restarting the counter')

    if not advance and stored_date == today and stored_seq > 0:
        return f'{today}{stored_seq:02d}'

    seq = stored_seq + 1 if stored_date == today else 1
    if seq > 99:
        # Two digits is the format. Past 99 builds in a day, keep counting rather
        # than silently reusing a number -- the string just gets one wider.
        warn(f'{seq} builds today; the number is now wider than 10 digits')

    try:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(f'{today} {seq}\n')
    except OSError as e:
        warn(f'could not write {COUNTER_FILE} ({e}); the number may repeat')

    return f'{today}{seq:02d}'


def _flag_sets_input_diag(text):
    return text == '-DINPUT_DIAG' or text.startswith('-DINPUT_DIAG=')


def _flags_set_input_diag(flags):
    """Both spellings PlatformIO accepts: -DINPUT_DIAG and the separated -D INPUT_DIAG."""
    flags = [str(f) for f in flags]
    for i, flag in enumerate(flags):
        if _flag_sets_input_diag(flag):
            return True
        if flag == '-D' and i + 1 < len(flags):
            name = flags[i + 1]
            if name == 'INPUT_DIAG' or name.startswith('INPUT_DIAG='):
                return True
    return False


def has_input_diag(env):
    """True when this build carries INPUT_DIAG.

    Every route the flag can arrive by is checked, because a diagnostic image
    named as a plain one is the mix-up this suffix exists to prevent, and the
    cost of it lands on the device: an SD write and a repro cycle against
    firmware that is not what it says it is.

    The routes differ in when they become visible. A pre: script runs before
    PlatformIO has folded build_flags into CPPDEFINES, so a -D from
    platformio.ini or platformio.local.ini is still only in the raw flag list;
    PLATFORMIO_BUILD_FLAGS reaches the compiler without passing through either
    at that point, and is only readable from the environment. Called again from
    the post action, CPPDEFINES has everything -- main() ORs the two, so a route
    that misses one is still caught by the other.
    """
    for define in env.get('CPPDEFINES', []):
        name = define[0] if isinstance(define, (list, tuple)) else define
        if str(name) == 'INPUT_DIAG':
            return True

    raw_flags = []
    for flag in env.get('BUILD_FLAGS', []) or []:
        try:
            raw_flags.extend(shlex.split(str(flag)))
        except ValueError:
            raw_flags.extend(str(flag).split())
    if _flags_set_input_diag(raw_flags):
        return True

    for var in ('PLATFORMIO_BUILD_FLAGS', 'PLATFORMIO_BUILD_SRC_FLAGS'):
        raw = os.environ.get(var)
        if not raw:
            continue
        try:
            flags = shlex.split(raw)
        except ValueError:
            # Unbalanced quotes: fall back to whitespace splitting rather than
            # reporting a diagnostic build as plain.
            flags = raw.split()
        if _flags_set_input_diag(flags):
            return True

    return False


def write_build_id_header(project_dir, build_id):
    """Publish the build number to the firmware through a generated header.

    A define would reach every translation unit and make each build a full
    rebuild; only the two files that write diagnostics need the string, so it
    goes in a header that only they include. It lives under lib/hal because
    that path is reachable from both lib and src, which src/util is not.
    """
    path = os.path.join(project_dir, 'lib', 'hal', 'ostBuildId.generated.h')
    body = f'#pragma once\n#define OST_BUILD_ID "{build_id}"\n'
    try:
        with open(path, 'r', encoding='utf-8') as f:
            if f.read() == body:
                return
    except OSError:
        pass
    try:
        with open(path, 'w', encoding='utf-8') as f:
            f.write(body)
    except OSError as e:
        warn(f'could not write {path} ({e}); the build id will be stale')


def copy_to_dist(project_dir, version, suffix, source):
    dist = os.path.join(project_dir, DIST_DIR)
    target = os.path.join(dist, f'crosspoint-OST-{version}{suffix}.bin')
    try:
        os.makedirs(dist, exist_ok=True)
        shutil.copyfile(source, target)
        print(f'OST build {version}{suffix} -> {os.path.relpath(target, project_dir)}')
    except OSError as e:
        warn(f'could not copy the image to {DIST_DIR} ({e})')


def main(env):
    project_dir = env.subst('$PROJECT_DIR')
    pioenv = env.subst('$PIOENV')
    board = BOARD_TAGS.get(pioenv, '')
    # Only a board with its own tag may share the day's number: its image name still differs.
    version = next_version(project_dir, advance=(pioenv not in BOARD_TAGS))
    diag_at_pre = has_input_diag(env)
    write_build_id_header(project_dir, f'{version}{board}{"-diag" if diag_at_pre else ""}')

    def post_action(target, source, env):
        # Re-check against the fully folded environment and keep whichever pass
        # saw the flag: the name has to be wrong in the safe direction.
        suffix = board + ('-diag' if (diag_at_pre or has_input_diag(env)) else '')
        copy_to_dist(project_dir, version, suffix, str(target[0]))

    env.AddPostAction('$BUILD_DIR/${PROGNAME}.bin', post_action)


# PlatformIO/SCons entry point — Import and env are SCons builtins injected at runtime.
try:
    Import('env')  # noqa: F821  # type: ignore[name-defined]
    main(env)  # noqa: F821  # type: ignore[name-defined]
except NameError:
    pass
