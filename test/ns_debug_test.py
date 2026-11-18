#!/usr/bin/env python3
"""Exercise real debugger sessions through the same file protocol as Lens."""
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import tempfile
import time

NS = str(Path(sys.argv[1]).resolve())


def atomic_write(path, text):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(text)
    temporary.replace(path)


with tempfile.TemporaryDirectory(prefix='ns-debug-') as temporary:
    root = Path(temporary).resolve()
    (root / 'src').mkdir()
    source = root / 'src/main.ns'
    source.write_text('''use std
fn add(a: i32, b: i32): i32 {
    return a + b
}
fn main() {
    let x = add(19, 23)
    print(`value {x}\\n`)
    assert x == 42
    let count = 0
    loop count < 3 {
        count = count + 1
    }
    assert count == 3
}
''')
    modes = ['eval', 'emu']
    if (platform.system(), platform.machine()) in [('Darwin', 'arm64'), ('Linux', 'x86_64')]:
        modes.append('exec')
    for mode in modes:
        # Explicit target selection, with a different default, must retain the
        # requested execution mode. This also catches accidental --cpu forcing.
        (root / 'ns.mod').write_text(f'''schema = "ns.mod/v2"
name = "debug_test"
type = "cli"
source = "src"
[[targets]]
name = "default"
entry = "main.ns"
target = "eval"
default = true
[[targets]]
name = "selected"
entry = "main.ns"
target = "{mode}"
''')
        session = root / 'bin' / mode
        session.mkdir(parents=True)
        atomic_write(session / 'alive', str(int(time.time())))
        # Use a real symlink, as macOS /tmp and /var commonly differ from the
        # compiler's getcwd spelling. Pending and stopped breakpoints must agree.
        alias = root / 'alias'
        if not alias.exists():
            try:
                alias.symlink_to(root, target_is_directory=True)
            except OSError:
                alias = root
        atomic_write(session / 'root', str(alias))
        atomic_write(session / 'breakpoints', f'{alias}/src/main.ns:3\n{alias}/src/main.ns:7\n')
        with (root / f'{mode}.log').open('w') as log:
            process = subprocess.Popen([NS, 'run', '--debug-session', str(session), 'selected'],
                                       cwd=root, stdout=log, stderr=log)
            previous = ''
            sequence = 0

            def wait_stop():
                global previous
                deadline = time.monotonic() + 60
                while time.monotonic() < deadline:
                    atomic_write(session / 'alive', str(int(time.time())))
                    state = (session / 'state').read_text() if (session / 'state').exists() else ''
                    if state and state != previous:
                        previous = state
                        return state.splitlines()
                    if process.poll() is not None:
                        raise AssertionError(f'{mode} exited early: ' + (root / f'{mode}.log').read_text())
                    time.sleep(.01)
                raise AssertionError(f'{mode}: timed out waiting for debugger')

            def command(action):
                global sequence
                sequence += 1
                atomic_write(session / 'command', f'{sequence} {action}')

            try:
                entry = wait_stop()
                assert entry[0].startswith(mode), entry
                assert entry[1] == f'{alias}/src/main.ns', entry
                command('continue')
                nested = wait_stop()
                assert nested[2] == '3', nested
                snapshot = '\n'.join(nested)
                assert '#registers\n' in snapshot and '#stack\n' in snapshot
                nested_context = snapshot.split('#frame\n', 1)[1].split('#registers\n', 1)[0]
                assert nested_context.strip()
                registers = snapshot.split('#registers\n', 1)[1].split('#stack\n', 1)[0]
                stack = snapshot.split('#stack\n', 1)[1].split('#stop ', 1)[0]
                if mode == 'eval':
                    assert 'no register file' in registers
                    assert '#0 add' in stack and '#1 main' in stack, stack
                    assert re.search(r'a (?:@\d+|\[immediate\]) = 0x0000000000000013', stack), stack
                    assert re.search(r'b (?:@\d+|\[immediate\]) = 0x0000000000000017', stack), stack
                elif mode == 'emu':
                    assert 'r0 = 0x0000000000000013 (19)' in registers, registers
                    assert 'r1 = 0x0000000000000017 (23)' in registers, registers
                    assert '#0 add' in stack and '#1 main' in stack, stack
                    assert 'Active register-stack window' in stack
                else:
                    assert 'Current native frame' in stack and 'Native call stack' in stack, stack
                    assert re.search(r'0x[0-9a-f]+  0x[0-9a-f]{16}', stack), stack
                    if platform.machine() == 'arm64':
                        assert re.search(r'x0\s+= 0x0000000000000013', registers), registers
                        assert re.search(r'x1\s+= 0x0000000000000017', registers), registers
                        assert 'q31 =' in registers and 'nzcv =' in registers, registers
                    else:
                        assert 'rdi = 0x0000000000000013' in registers, registers
                        assert 'rsi = 0x0000000000000017' in registers, registers
                        assert 'xmm15 =' in registers and 'rflags =' in registers, registers
                command('continue')
                stopped = wait_stop()
                assert stopped[2] == '7', stopped
                main_context = '\n'.join(stopped).split('#frame\n', 1)[1].split('#registers\n', 1)[0]
                if mode == 'emu':
                    assert nested_context != main_context
                    entry_context = '\n'.join(entry).split('#frame\n', 1)[1].split('#registers\n', 1)[0]
                    assert main_context == entry_context
                if mode == 'exec':
                    assert main_context == nested_context
                if mode == 'emu':
                    assert 'ns_rt_' in '\n'.join(stopped[3:]), stopped
                if mode == 'exec':
                    assert stopped[3].startswith('0x'), stopped
                command('step')
                assert wait_stop()[2] == '8'
                command('continue')
                assert process.wait(timeout=10) == 0
                assert 'value 42' in (root / f'{mode}.log').read_text()
            finally:
                if process.poll() is None:
                    process.kill()
                process.wait()
        # An ordinary run after a debug compilation must remain uninstrumented.
        normal = subprocess.run([NS, 'run', 'selected'], cwd=root, capture_output=True, text=True, timeout=60)
        assert normal.returncode == 0, normal.stderr
        assert 'value 42' in normal.stdout
        print(f'PASS: {mode} entry, symlinked source breakpoint, step, continue, loop, register/stack snapshots and ordinary run')

        # Reusing the same register window for a later invocation must not
        # reuse its activation identity. Native physical registers instead
        # keep the same identity for the lifetime of their OS thread.
        saved_source = source.read_text()
        source.write_text('fn add(a: i32, b: i32): i32 {\n    return a + b\n}\nfn main() {\n    let first = add(1, 2)\n    let second = add(first, 3)\n    assert second == 6\n}\n')
        session = session / 'activations'
        session.mkdir()
        atomic_write(session / 'alive', str(int(time.time())))
        atomic_write(session / 'breakpoints', f'{source}:2\n')
        with (root / f'{mode}.log').open('a') as log:
            process = subprocess.Popen([NS, 'run', '--debug-session', str(session), 'selected'], cwd=root, stdout=log, stderr=log)
            previous = ''
            sequence = 0
            try:
                wait_stop()
                command('continue')
                first_call = wait_stop()
                assert first_call[2] == '2', first_call
                first_identity = '\n'.join(first_call).split('#frame\n', 1)[1].split('#registers\n', 1)[0]
                command('continue')
                second_call = wait_stop()
                assert second_call[2] == '2', second_call
                second_identity = '\n'.join(second_call).split('#frame\n', 1)[1].split('#registers\n', 1)[0]
                assert (first_identity != second_identity) == (mode == 'emu')
                command('continue')
                assert process.wait(timeout=10) == 0
            finally:
                if process.poll() is None:
                    process.kill()
                process.wait()
                source.write_text(saved_source)
        print(f'PASS: {mode} register identity across repeated calls')

        original = source.read_text()
        source.write_text('fn main() {\n    let count = 0\n    loop true {\n        count = count + 1\n    }\n}\n')
        pause_session = session / 'pause'
        pause_session.mkdir()
        atomic_write(pause_session / 'alive', str(int(time.time())))
        with (root / f'{mode}-pause.log').open('w') as log:
            process = subprocess.Popen([NS, 'run', '--debug-session', str(pause_session), 'selected'],
                                       cwd=root, stdout=log, stderr=log)
            first = ''
            try:
                deadline = time.monotonic() + 60
                while time.monotonic() < deadline:
                    atomic_write(pause_session / 'alive', str(int(time.time())))
                    if (pause_session / 'state').exists():
                        first = (pause_session / 'state').read_text()
                        break
                    assert process.poll() is None
                    time.sleep(.01)
                assert first, f'{mode}: missing entry stop'
                atomic_write(pause_session / 'command', '1 continue')
                time.sleep(.1)
                atomic_write(pause_session / 'command', '2 pause')
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    if (pause_session / 'state').read_text() != first:
                        break
                    time.sleep(.01)
                else:
                    raise AssertionError(f'{mode}: pause did not stop the running loop')
                atomic_write(pause_session / 'command', '3 stop')
                assert process.wait(timeout=10) == 0
            finally:
                if process.poll() is None:
                    process.kill()
                process.wait()
                source.write_text(original)
        print(f'PASS: {mode} pause and stop a running loop')

    (root / 'ns.mod').write_text('''schema = "ns.mod/v2"
name = "browser"
type = "app"
source = "src"
entry = "main.ns"
target = "wasm"
''')
    unsupported = subprocess.run([NS, 'run', '--debug-session', str(session)], cwd=root,
                                 capture_output=True, text=True, timeout=10)
    assert unsupported.returncode != 0
    assert 'browser targets do not yet support Lens breakpoints' in unsupported.stderr
    print('PASS: browser debug fails explicitly before starting the development server')
