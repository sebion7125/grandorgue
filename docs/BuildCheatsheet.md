# Build-/Run-Cheatsheet

Schnellübersicht zum Bauen (Win64 Cross/Native Linux) und zur Navigation der Build-Artefakte.

## Win64 Cross-Build (MinGW unter Linux)
Voraussetzung: Cross-Toolchain/Deps über die Skripte bereitgestellt.

1) In das Win64-Build-Verzeichnis wechseln:
```
cd build-scripts/for-win64
```

2) (Empfohlen bei frischem Setup/Toolchain-Änderungen)
```
./update-build-on-linux.sh
```

3) Bauen:
```
./build-on-linux.sh
```

4) Ergebnis:
- Artefakte unter: `build-scripts/for-win64/build/`
- Typisch: Binärdateien/Installer/Package (je nach Skriptkonfiguration)

Optional: Ausführen mit Wine (falls benötigt)
```
wine path/to/GrandOrgue.exe
```

## Native Linux-Build
1) In das Linux-Build-Verzeichnis wechseln:
```
cd build-scripts/for-linux
```

2) Bauen:
```
./build-on-linux.sh
```

3) Ergebnis:
- Artefakte unter: `build-scripts/for-linux/build/`

4) Tests:
```
./do-tests.sh
```

## Code-Navigation (clangd)
In `.vscode/settings.json` ist konfiguriert:
```
"clangd.arguments": ["--compile-commands-dir=build/win64"]
```
Stelle sicher, dass `compile_commands.json` dort zu finden ist oder dorthin verlinkt wird. Zwei mögliche Wege:

- Einstellung an tatsächlichen Pfad anpassen (falls die Build-Skripte die Datei woanders erzeugen)
- Oder Symlink anlegen, z. B.:
```
mkdir -p build/win64
ln -sf /pfad/zur/compile_commands.json build/win64/compile_commands.json
```

Hinweis: Ob `compile_commands.json` erzeugt wird, hängt von den CMake-Flags der Build-Skripte ab (z. B. `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`). Prüfe das Ergebnis nach dem Build.

## Nützliche Pfade
- Win64 Artefakte: `build-scripts/for-win64/build/`
- Linux Artefakte: `build-scripts/for-linux/build/`
- Haupt-CMake: `CMakeLists.txt`
- Audio-Engine Einstieg: `src/grandorgue/sound/GOSoundEngine.*`
- App-Einstieg: `src/grandorgue/GOApp.cpp`

Weitere Struktur-Infos: `docs/RepoMap.md`
