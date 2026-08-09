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

Injects OST_VERSION, and copies the built image to dist/ under a name carrying
the same number. Diagnostic builds (INPUT_DIAG) get a -diag suffix so the two
cannot be confused on the card.
"""

import datetime
import os
import shutil
import sys

COUNTER_FILE = '.ost-build'
DIST_DIR = 'dist'


def warn(msg):
    print(f'WARNING [ost_version.py]: {msg}', file=sys.stderr)


def next_version(project_dir):
    """Return YYYYMMDDNN, advancing the per-day counter."""
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


def has_input_diag(env):
    """True when this build carries INPUT_DIAG.

    Checked in both places it can appear. A pre: script runs before PlatformIO has
    finished folding build_flags into CPPDEFINES, so a -D from platformio.local.ini
    is still only in the raw flag list at this point -- reading CPPDEFINES alone
    reported a diagnostic build as a plain one, which is exactly the mix-up on the
    SD card this naming exists to prevent.
    """
    for define in env.get('CPPDEFINES', []):
        name = define[0] if isinstance(define, (list, tuple)) else define
        if str(name) == 'INPUT_DIAG':
            return True

    flags = env.get('BUILD_FLAGS', []) or []
    for flag in flags:
        text = str(flag)
        if text == '-DINPUT_DIAG' or text.startswith('-DINPUT_DIAG='):
            return True
    return False


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
    version = next_version(project_dir)
    suffix = '-diag' if has_input_diag(env) else ''

    env.Append(CPPDEFINES=[('OST_VERSION', f'\\"{version}\\"')])

    env.AddPostAction(
        '$BUILD_DIR/${PROGNAME}.bin',
        lambda target, source, env: copy_to_dist(
            project_dir, version, suffix, str(target[0])
        ),
    )


# PlatformIO/SCons entry point — Import and env are SCons builtins injected at runtime.
try:
    Import('env')  # noqa: F821  # type: ignore[name-defined]
    main(env)  # noqa: F821  # type: ignore[name-defined]
except NameError:
    pass
