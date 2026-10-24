Kurzüberblick — welche Änderungen ich gemacht habe

- Ziel: temporäre Laufzeit‑Logs einfügen, um Tremulant/Windchest/Release‑Gain‑Berechnungen sichtbar zu machen.
- Dateien geändert:
  - src/grandorgue/sound/scheduler/GOSoundTremulantTask.cpp
    - wxLogDebug -> wxLogMessage nach m_Volume‑Berechnung
  - src/grandorgue/sound/scheduler/GOSoundWindchestTask.cpp
    - wxLogDebug -> wxLogMessage nach windchest‑Volume‑Berechnung
  - src/grandorgue/sound/GOSoundEngine.cpp
    - mehrere wxLogDebug → wxLogMessage (CreateReleaseSampler): handle/release_section/crossFadeSamples, detached release gain_target, attack/gain_delta

Warum du bisher nichts im GO‑Logfenster gesehen hast
- wxLogDebug Meldungen werden nur ausgegeben, wenn das Log‑Level und der ActiveTarget dies zulassen.
- GrandOrgue setzt in GOApp das ActiveTarget auf ein GOLog‑Objekt; je nach LogLevel können Debug‑Meldungen (wxLogDebug) unterdrückt werden.

Was ich geändert habe, damit Meldungen normalerweise sichtbar sind
- Ich habe die Ausgaben auf wxLogMessage (höheres Level) umgestellt — diese Meldungen sollten im GO‑Logfenster erscheinen, sofern das Programm das GOLog als ActiveTarget setzt.

Wie du lokal die Logs erzeugst (kurze Anleitung)
1) Baue das Projekt wie gewohnt auf deinem Linux/Windows‑System.
2) Starte GrandOrgue aus einem Terminal (für Linux: ./bin/GrandOrgue). Wenn du in Windows testest, starte die ausführbare Datei entsprechend.
3) Öffne das Log‑Fenster in GrandOrgue (Menü: Hilfe → Log messages). Alternativ: beobachte die Terminalausgabe.
4) Spiele eine Note (Attack) und lasse sie los (Release). Die folgenden Meldungen werden beim Ablauf erzeugt, falls der Codepfad durchlaufen wird:
   - GOSoundTremulantTask: m_Volume=%f
   - GOSoundWindchestTask: windchestVol=%f
   - CreateReleaseSampler: (mehrere Meldungen mit crossFadeSamples, vol, gain_target, time/attack_duration/gain_delta)

Wenn du keine Meldungen siehst
- Prüfe, ob das Log‑Fenster geöffnet ist und ob GOApp das ActiveTarget gesetzt hat (normalerweise automatisch beim Start). Wenn du ein spezielles Start‑Flag oder ein Test‑Binary benutzt, könnte das Verhalten abweichen.
- Falls du weiterhin keine Meldungen siehst, führe den Test im Terminal aus und leite die Standardausgabe in eine Datei um, damit wir prüfen können, ob wxWidgets‑Logausgaben überhaupt an stdout/stderr gehen.

Was ich nicht gemacht habe (vorsichtiges Vorgehen)
- Keine produktiven Änderungen an der Audio‑Logik selbst — nur temporäre Logausgaben.
- Keine dauerhaften Debug‑Statements entfernt; nach Analyse sollten wir die Zeilen wieder sauber entfernen oder sie hinter einem #ifdef DEBUG schützen.

Next steps (dein Wunsch)
- Du baust und testest lokal (Windows oder Linux) und schickst mir die relevanten Log‑Auszüge — ich analysiere sie und schlage gezielte Codefixes vor.
- Alternativ kann ich hier weiter automatisiert testen, falls du mir erlaubst, das Build/Run auf dieser Linux‑Maschine auszuführen.

Bei Fragen oder wenn du die Log‑Zeilen nicht bekommst, schick mir bitte:
- die Ausgabe deines Terminal‑Starts von GrandOrgue (erste 200 Zeilen)
- und eine kurze Beschreibung, welche Note / welche Orgel‑Konfiguration du gespielt hast.

— Ende
