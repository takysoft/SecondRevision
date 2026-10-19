"""Writes SecondRevision.settings.ini.example straight out of the renderer's own template.

The template (GIniTemplate in src/RevisionDX12/RenderDevice.cpp) is what the renderer really writes on a first
start, so an example built from anything else would drift away from it the first time someone edits the source.
The values filled in are the defaults - which are the Recommended picture (RdxDefault* in the same file).
first_start_values() is that filling on its own: scripts/settings_refresh.py takes a missing setting's value from it.

Run: python scripts/settings_example.py
"""
import io
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src', 'RevisionDX12', 'RenderDevice.cpp')
MATH = os.path.join(ROOT, 'src', 'RevisionDX12', 'RenderMath.h')
OUT = os.path.join(ROOT, 'SecondRevision.settings.ini.example')

text = io.open(SRC, encoding='utf-8', newline='').read()

# The template: every string literal between "GIniTemplate =" and the statement's closing semicolon. The end is the
# only quote in the whole statement that a semicolon and a line break follow - every other line ends with a quote
# and a line break alone. (Looking for the first ';' fails: the template's own lines start with one.)
start = text.index('static const char* const GIniTemplate =')
end = text.index('";\n', start) + 1
pieces = re.findall(r'"((?:[^"\\]|\\.)*)"', text[start:end])
template = ''.join(pieces)
# Undo the C escapes we actually use in it.
template = template.replace('\\n', '\n').replace('\\"', '"').replace('\\\\', '\\')


def default(name):
    """The value of one RdxDefault<name> constant, as it is spelled in the source."""
    m = re.search(r'RdxDefault%s\s*=\s*(?:TEXT\(")?([^";\n]+?)"?\)?;' % name, text)
    if not m:
        raise SystemExit('default not found: RdxDefault%s' % name)
    return m.group(1).strip()


def math_text():
    return io.open(MATH, encoding='utf-8', newline='').read()


def menu_key():
    """The default menu key as the settings file spells it: RdxMath::MenuKeyDefault, named by the first row of
    MenuKeyNames that carries it - which is what RdxMath::MenuKeyIniName writes on a first start."""
    math = math_text()
    vk = re.search(r'MenuKeyDefault\s*=\s*(0x[0-9A-Fa-f]+)', math)
    if not vk:
        raise SystemExit('default not found: MenuKeyDefault')
    row = re.search(r'\{\s*%s,\s*"([^"]+)"' % vk.group(1), math)
    if not row:
        raise SystemExit('no name for the default menu key %s' % vk.group(1))
    return row.group(1)


BOOLS = {'1': 'On', '0': 'Off'}


def value(name):
    """A default as the settings file spells it: a number without the source's f, a switch as On or Off."""
    v = default(name)
    if re.fullmatch(r'[0-9]+[.][0-9]+f', v):
        return v[:-1]
    return BOOLS.get(v, v)


def water():
    """RdxDefaultWater as the settings file spells it: the enum value's name after its prefix (RdxMath::Water_Off)."""
    return default('Water').split('Water_')[-1]


def reflections():
    """RdxDefaultReflections the same way (RdxMath::Reflections_Default)."""
    return default('Reflections').split('Reflections_')[-1]


def named(name, table):
    """RdxDefault<name> as the renderer writes it on a first start: by the name of the value it reads the default as -
    the entry of RdxMath::<table>() that matches it, case aside. A default that matches none stops the script."""
    v = default(name)
    m = re.search(r'%s\(\)\s*\{\s*static const char\* const Names\[\w+\]\s*=\s*\{([^}]*)\}' % table, math_text())
    if not m:
        raise SystemExit('names not found: RdxMath::%s' % table)
    for n in re.findall(r'"([^"]*)"', m.group(1)):
        if n.lower() == v.lower():
            return n
    raise SystemExit('RdxDefault%s is %s - not one of RdxMath::%s' % (name, v, table))


def first_start_values():
    """Every placeholder of the template with what the renderer writes there on a first start (WriteOwnIni's Names
    and Values in RenderDevice.cpp)."""
    return {
        'PRESET':        value('Preset'),
        'AA':            named('AntiAliasing', 'AaChoiceNames'),
        'HDR':           value('Hdr'),
        'SHARP':         value('FsrSharpness'),
        'UPSCALE':       named('Upscale', 'UpscaleNames'),
        'FRAMEGEN':      value('FrameGeneration'),
        'ANTILAG':       value('AntiLag'),
        'SQ':            value('ShadowQuality'),
        'VERT':          value('ShadowVertical'),
        'READOUT':       value('Readout'),
        'BORDERLESS':    value('Borderless'),
        'EXCLUSIVE':     value('Exclusive'),
        'GPUCONV':       value('GpuConvert'),
        'GPU':           value('Gpu'),
        'AO':            value('Ao'),
        'DEBUGKEY':      menu_key(),
        'DPIFIX':        value('DpiFix'),
        'FPSLIMIT':      value('FpsLimit'),
        'VSYNC':         value('Vsync'),
        'WATER':         water(),
        'REFLECTIONS':   reflections(),
        'GEOMETRY':      named('Geometry', 'GeometryNames'),
        'SOFTPARTICLES': value('SoftParticles'),
        'VOLUMETRICS':   value('Volumetrics'),
        'MOTIONBLUR':    value('MotionBlur'),
        'ABERRATION':    value('ChromaticAberration'),
        'VIGNETTE':      value('Vignette'),
        'FILMGRAIN':     value('FilmGrain'),
        'BLOOM':         named('Bloom', 'BloomNames'),
        'LENSFLARE':     named('LensFlare', 'LensFlareNames'),
        'GODRAYS':       value('GodRays'),
        'DEPTHOFFIELD':  value('DepthOfField'),
        'ENDLESSWATER':  value('EndlessWater'),
        'FIRE3D':        value('Fire3D'),
        'GRASS3D':       value('Grass3D'),
        'TREES3D':       value('Trees3D'),
        'WIND':          value('Wind'),
        'FOGROOMS':      'Fixed' if value('FogRooms') == 'On' else 'Original',
        'FORCEFIELD':    named('ForceField', 'ForceFieldNames'),
        'FLASHLIGHTBEAM': named('Flashlight', 'FlashlightNames'),
        'HDTPHOLOFIX':   named('HdtpHoloFix', 'HoloFixNames'),
        'REVISIONSPLASHSCREENFIX': value('SplashFix'),
        'LOADFIX':       value('LoadFix'),
        'MONITORFIX':    value('MonitorFix'),
        'SAFEMODE':      value('SafeMode'),
        'LOGLEVEL':      value('LogLevel'),
    }


def main():
    out = template
    for key, val in first_start_values().items():
        out = out.replace('@%s@' % key, val)

    missing = re.findall(r'@([A-Z0-9]+)@', out)
    if missing:
        raise SystemExit('placeholder left unfilled: %s' % ', '.join(missing))

    # Byte for byte what the renderer writes on a first start: its template's line ends are LF.
    io.open(OUT, 'w', encoding='utf-8', newline='\n').write(out)
    print('written: %s (%i lines)' % (OUT, out.count('\n')))
    for line in out.splitlines():
        if line and not line.startswith(';') and not line.startswith('['):
            print('  ' + line)


if __name__ == '__main__':
    main()
