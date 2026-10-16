# -*- mode: python ; coding: utf-8 -*-
# Build on Windows (x64):
#   pyinstaller MacroHelper.spec
# Output: dist/MacroHelper.exe  → copy next to SingleUpdate.exe

block_cipher = None

a = Analysis(
    ['macro_helper/__main__.py'],
    pathex=['.'],
    binaries=[],
    datas=[],
    hiddenimports=[
        'comtypes.stream',
        'comtypes.gen',
        'pywinauto.uia_element_info',
        'pywinauto.controls.uiawrapper',
        'pynput.keyboard._win32',
        'pynput.mouse._win32',
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.zipfiles,
    a.datas,
    [],
    name='MacroHelper',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,  # --noconsole: GUI record; play still prints via AttachConsole when spawned
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
