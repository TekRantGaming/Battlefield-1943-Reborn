"""The Python the Windows server package carries (TekRantGaming, 13 Nov 2026: a host should not have to install
anything; before, Start failed on a PC with only the Microsoft Store's python shortcut).

Made from the Python that runs this script (python.org's Windows build, the same files its embeddable download has):
the interpreter, its DLL, the extension modules the manager and the hosts use, and the standard library in one zip.
No pip, no tests, no Tk. A python3XX._pth beside it keeps it to itself: nothing of a Python installed on the PC, and
no environment variable, changes what it loads.

    python tools/server_package/make_python_runtime.py <output folder>

The files are python.org's own (signed by the Python Software Foundation); their licence goes along (LICENSE.txt).
"""
import os
import shutil
import sys
import zipfile

# Not for a server: the test suites, the Tk user interface, IDLE, the installers, the documentation's data.
SKIP_LIB = {'test', 'tests', 'tkinter', 'idlelib', 'turtledemo', 'ensurepip', 'lib2to3', 'pydoc_data', 'site-packages',
            'venv', 'sqlite3', 'unittest', 'msilib', 'curses', '__pycache__', 'distutils', 'idle_test'}
SKIP_FILES = {'turtle.py', 'doctest.py', 'pydoc.py', 'this.py', 'antigravity.py'}
SKIP_DLLS = ('_tkinter', 'tcl', 'tk8', '_test', '_ctypes_test', 'sqlite3', '_sqlite3', '_msi', 'winsound', 'zlib1',
             '_zoneinfo', '_decimal', '_multiprocessing', 'pyexpat', '_elementtree', '_uuid', '_wmi')


def main():
    if os.name != 'nt':
        sys.exit('This makes the Windows package\'s Python: run it with a Windows Python.')
    out = os.path.abspath(sys.argv[1])
    source = sys.base_prefix
    name = 'python%d%d' % sys.version_info[:2]
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    for f in ('python.exe', 'pythonw.exe', name + '.dll', 'python3.dll', 'vcruntime140.dll', 'vcruntime140_1.dll',
              'LICENSE.txt'):
        path = os.path.join(source, f)
        if not os.path.exists(path):
            sys.exit('%s is not in %s: use python.org\'s Python for Windows.' % (f, source))
        shutil.copy2(path, os.path.join(out, f))
    dlls = os.path.join(source, 'DLLs')
    for f in sorted(os.listdir(dlls)):
        lower = f.lower()
        if not lower.endswith(('.pyd', '.dll')) or lower.startswith(SKIP_DLLS):
            continue
        shutil.copy2(os.path.join(dlls, f), os.path.join(out, f))
    lib = os.path.join(source, 'Lib')
    count = 0
    with zipfile.ZipFile(os.path.join(out, name + '.zip'), 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for folder, folders, files in os.walk(lib):
            folders[:] = sorted(d for d in folders if d not in SKIP_LIB)
            relative = os.path.relpath(folder, lib)
            for f in sorted(files):
                if not f.endswith('.py') or (relative == '.' and f in SKIP_FILES):
                    continue
                z.write(os.path.join(folder, f), f if relative == '.' else os.path.join(relative, f))
                count += 1
    # The interpreter's own search path, and nothing else (no registry, no environment, no user site).
    with open(os.path.join(out, name + '._pth'), 'w', newline='\n') as f:
        f.write(name + '.zip\n.\n')
    size = sum(os.path.getsize(os.path.join(out, f)) for f in os.listdir(out))
    print('%s: Python %s, %d library files, %.1f MB' % (out, sys.version.split()[0], count, size / 1048576))


if __name__ == '__main__':
    main()
