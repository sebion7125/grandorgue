# GrandOrgue-Odyssey.md

Dieses Dokument sammelt Erkenntnisse, Fixes und Experimente aus der gemeinsamen Arbeit an GrandOrgue. Es ist als **Meta-Logbuch** gedacht, das Querverweise zu bestehenden RepoMap-Dokumenten herstellt und so einer KI-Integration ermöglicht, Wissen automatisch in den Kontext einzuordnen.

Hinweis: Eine detaillierte Version dieses Meta-Logbuchs findet sich hier: [GrandOrgue-Odyssey (Details)](grand_orgue_odyssey_details.md)

---

## 🔧 Build & Toolchain

* **Cross-Compilation unter Linux → Windows**:

  * Toolchain-Datei: `/home/ubuntu/mingw-toolchain.cmake`
  * Eigenheiten mit `wxWidgets`:

    * Bugfix: `CompareNCaseInsensitive` im Scintilla-Modul.
    * Installation variierte: `$HOME/wxWidgets-mingw/install` vs. systemweit.
  * Workaround: `find_package(wxWidgets)` in `CMakeLists.txt` durch manuelle Definition ersetzen.
  * Post-Build: `patch_compile_commands.py` anhängen, um fehlende `-isystem`-Includes für Clang/VS Code zu ergänzen.
* **Build-Skript-Erweiterungen**:

  * Automatisches Einfügen fehlender Include-Pfade (`wx/setup.h`).
  * Anpassbarer Pfad zu ASIO-Treibern.

👉 Siehe auch: [BuildCheatsheet.md](BuildCheatsheet.md)

---

## 🎚️ Sound Engine & DSP

* **Fade-Modelle**:

  * Linearer Fade (Basis).
  * Sinus-Fade (`sin^2`) → sehr glatt, aber Pegelberg möglich.
  * Equal Power Crossfade diskutiert.
  * Parabelmodell für Attack/Release entwickelt.
* **Bekannte Fixes**:

  * Bug: Bei `nFrames==0` im Sinus-Fade → Release-Sample stumm. Fix: `fadeFactor=1.0` setzen.
  * Entfernen unnatürlicher Fade-Outs bei extrem kurzen Tastenanschlägen → realistischer mit Release-Samples.
* **Experimental**:

  * Attack-Modell mit `t_max`-Bestimmung:

    * Methode 1: Steigungsmethode (Slope → Zero-Crossing).
    * Methode 2: Plateauerkennung.
    * Methode 3: Top-10 Maxima + Parabelfit, Auswahl per MSE.
  * `GO_Attack_Parameters.cpp`: Tabellen für `curvature_by_midi` und `attack_time_by_midi` generiert.

👉 Siehe auch: [SoundEngine.md](RepoMap/SoundEngine.md), [CrossfadeIntegration.md](CrossfadeIntegration.md)

---

## 📦 ODF & Daten

* **Kombination Dry/Wet-Presets**:

  * Textbasiertes Parsen von `.organ` Dateien (kein XML!).
  * Vorgehen:

    * Dry-Ranks + Windchests übernehmen.
    * Neue Enclosure für Dry-Ranks anlegen.
    * Stops im Wet-Preset mit `DryRankID` verknüpfen.
  * Besonderheit: `EnclosureID` → modernisiert in `NumberOfEnclosures` + `EnclosureNNN`.

👉 Siehe auch: [ODFLoading.md](RepoMap/ODFLoading.md)

---

## 🧪 Experimental Features

* Crossfade-Demos mit `GO_VERSION_EXTRA` getaggt (z. B. `XFadeDemo`).
* GUI/Build-Erweiterungen für Debugging (`--clean`, `--reconfigure`).
* Eigene Analyse-Skripte zur Messung von Attack/Release:

  * Envelope-Glättung (50 ms Fenster).
  * Parabelkrümmung als relatives Maß: `a = -p / t_max^2`.

---

## 🧰 Externe Tools & Skripte

- Python-Toolchain für ReleaseGain: Skripts zur Einspeisung des ReleaseGain-Modells mit Messdaten aus einem Sampleset (außerhalb dieses Repos entwickelt). Details und Diskussionsverlauf: [Konversationsmitschnitte](GO_Project_Chats.md).
- GO_API_Context_Bundle.zip: Vollständigeres API/Projekt-Kontext-Bundle mit begleitenden Artefakten. Datei: [GO_API_Context_Bundle.zip](GO_API_Context_Bundle.zip).

## 🚨 Known Issues

* **Build**:

  * Fehlende Includes (`wx/setup.h`) → Workaround via `patch_compile_commands.py`.
  * Unterschiedliche Installationspfade von `wxWidgets` erschweren Crossbuild.
* **Sound Engine**:

  * Kurze Tastenanschläge: Release-Sounds können abgeschnitten wirken.
  * Sinus-Fade ohne Guard-Case bei `nFrames==0`.

---

## 📑 Verweise

* [Konversationsmitschnitte](GO_Project_Chats.md)
* [GrandOrgue-Odyssey (Details)](grand_orgue_odyssey_details.md)
* [API Context Bundle](GO_API_Context_Bundle.zip)
* [BuildCheatsheet.md](BuildCheatsheet.md)
* [SoundEngine.md](RepoMap/SoundEngine.md)
* [CrossfadeIntegration.md](CrossfadeIntegration.md)
* [ODFLoading.md](RepoMap/ODFLoading.md)
