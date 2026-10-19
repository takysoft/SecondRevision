"""Where the game is on this machine: the one place every Python script asks.

In this order: the RDX_GAME_ROOT environment variable; the first line of game-path.txt in the project's root (this
machine's own file, not in the repository); Windows' own list of installed programs, whichever shop or installer put
the game there; and Steam's libraries, for a library Windows has no entry for. Where none of them has it the script
stops and says how to name the folder - nothing is guessed. The folder is the game's own: the one that holds
Revision\\System\\Revision.exe. scripts\\game.ps1 says the same for the PowerShell scripts.

    import game_path                       # GAME_ROOT, GAME_SYSTEM, GAME_SAVES, GAME_FOUND_BY, PROJECT_ROOT
    python scripts/game_path.py            # prints what it found, and how
    python scripts/game_path.py --root     # the game's folder alone, for the .cmd scripts
"""
import os
import re

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_PATH_FILE = os.path.join(PROJECT_ROOT, 'game-path.txt')


def _holds(path):
    return bool(path) and os.path.isfile(os.path.join(path, 'Revision', 'System', 'Revision.exe'))


def _installed_folders():
    """Every installed program's folder, from both halves of the registry and for this user alone."""
    try:
        import winreg
    except ImportError:
        return
    for view in (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY):
        for hive in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
            try:
                programs = winreg.OpenKey(hive, r'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', 0,
                                          winreg.KEY_READ | view)
            except OSError:
                continue
            with programs:
                for index in range(winreg.QueryInfoKey(programs)[0]):
                    try:
                        with winreg.OpenKey(programs, winreg.EnumKey(programs, index)) as entry:
                            folder = winreg.QueryValueEx(entry, 'InstallLocation')[0]
                    except OSError:
                        continue
                    if isinstance(folder, str) and folder.strip():
                        yield folder.strip().strip('"').rstrip('\\')


def _steam_libraries():
    """Steam's own folder and every library its list names, as far as the registry and the list can be read."""
    try:
        import winreg
    except ImportError:
        return []
    steam = None
    for hive, key, name in ((winreg.HKEY_CURRENT_USER, r'Software\Valve\Steam', 'SteamPath'),
                            (winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\WOW6432Node\Valve\Steam', 'InstallPath')):
        try:
            with winreg.OpenKey(hive, key) as handle:
                steam = winreg.QueryValueEx(handle, name)[0]
        except OSError:
            continue
        if steam:
            break
    if not steam:
        return []
    steam = steam.replace('/', '\\')
    libraries = [steam]
    try:
        with open(os.path.join(steam, 'steamapps', 'libraryfolders.vdf'), encoding='utf-8', errors='replace') as f:
            for line in f:
                found = re.search(r'"path"\s+"([^"]+)"', line)
                if found:
                    libraries.append(found.group(1).replace('\\\\', '\\'))
    except OSError:
        pass
    return libraries


def find_game_root():
    """The game's folder and how it was found; (None, None) where it was not."""
    asked = os.environ.get('RDX_GAME_ROOT', '').rstrip('\\')
    if _holds(asked):
        return asked, 'the RDX_GAME_ROOT variable'
    try:
        with open(_PATH_FILE, encoding='utf-8-sig') as f:
            line = f.readline().strip().strip('"').rstrip('\\')
        if _holds(line):
            return line, 'game-path.txt'
    except OSError:
        pass
    # An entry may name the game's folder or the Revision folder inside it.
    for folder in _installed_folders():
        for candidate in (folder, os.path.dirname(folder)):
            if _holds(candidate):
                return candidate, 'the list of installed programs'
    for library in _steam_libraries():
        game = os.path.join(library, 'steamapps', 'common', 'Deus Ex')
        if _holds(game):
            return game, "Steam's libraries"
    return None, None


GAME_ROOT, GAME_FOUND_BY = find_game_root()
if GAME_ROOT is None:
    raise SystemExit('the game was not found on this machine. Say where it is: put the folder that holds '
                     'Revision\\System\\Revision.exe into the first line of %s, or into the RDX_GAME_ROOT '
                     'environment variable.' % _PATH_FILE)
GAME_SYSTEM = os.path.join(GAME_ROOT, 'Revision', 'System')
GAME_SAVES = os.path.join(GAME_ROOT, 'Revision', 'Save')

if __name__ == '__main__':
    import sys
    if '--root' in sys.argv:
        # For the .cmd scripts: the folder alone, a line they can take as it is.
        print(GAME_ROOT)
    else:
        print('the game:    %s   (found by %s)' % (GAME_ROOT, GAME_FOUND_BY))
        print('the project: %s' % PROJECT_ROOT)
