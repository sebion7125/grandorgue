# Model – Vollanalyse (komplettierte Fassung)

Ziel: Kompakter Überblick über das Modell in `src/grandorgue/model/`, mit Verantwortlichkeiten, Beziehungen und konkreten Implementierungs-Hotspots (Symbole, Funktionen), damit man schnell mit clangd Sprünge setzen kann.

Kurz:
- Rolle: Repräsentation der Orgelstruktur (Windchests, Ranks, Pipes, Manuals, Stops, Couplers, Enclosures, Tremulants), Lebenszyklus der Modell-Objekte, Registrierung in Handler-Listen, Cache/Load-Pfade, Schnittstellen zur Sound-Engine und GUI/MIDI.
- Fokus: Wo Objekte erzeugt/konfiguriert/verbunden werden, wie Key/Stop/Coupler → Pipe → Engine Fluss abläuft, Cache- und Ladepfade.

1) Wichtigste Dateien & Kernverantwortungen (Kurz)
- GOOrganModel.{h,cpp}
  - Aggregation aller Modell-Container: Windchests, Manuals, Enclosures, Switches, Tremulants, Ranks, Pistons, Couplers, Generals.
  - Entrypoint beim Laden: `GOOrganModel::Load(cfg)` (vom Controller aufgerufen).
  - Enthält Policies/Flags für Kombinationen, Divisionals etc.

- GOEventHandlerList.{h,cpp}
  - Zentraler Registrierungscontainer (CacheObjects, ReferencingObjects, SaveableObjects, SoundStateHandlers, MidiEventHandlers, ...).
  - Wird von `GOOrganModel` geerbt — damit kann der Controller/Distributor über Collections iterieren.

- GOPipe.{h,cpp}
  - Abstraktion aller Pipes. Handhabt multi-reference velocities (Register- oder REF-Mechanik) und ruft `VelocityChanged` (virtuell) auf.
  - `GOPipe::SetVelocity` berechnet die neue effektive Velocity und ruft `m_Rank->SendKey` + `VelocityChanged`.

- GOSoundingPipe.{h,cpp}
  - Sample-basierte Pipe (SoundProviderWave). Wichtige Implementierungen:
    - `GOSoundingPipe::Load/Init/LoadData/LoadCache/SaveCache/UpdateHash`
    - `GOSoundingPipe::VelocityChanged(...)` — Start/Stop/Update der Sampler:
      - Bei Key-Press (Instanzen==0 && velocity>0) ruft Pipe -> Engine: `GOSoundEngine::StartPipeSample(&m_SoundProvider, ...)` zurück, erhält `GOSoundSampler*`.
      - Bei Key-Release (m_Instances > 0 && velocity == 0) ruft Engine `StopSample(&m_SoundProvider, p_CurrentLoopSampler)`; für unabhängige Releases ggf. Start eines Release-Samples.
      - Bei Velocity-Änderung während gehaltenem Ton ruft Engine `UpdateVelocity(...)`.
    - Tuning/Temperament/Amplitude werden über PipeConfigNode und `m_SoundProvider` gesetzt (`SetTuning`, `SetAmplitude`, `SetReleaseTail`, ...).
  - Wichtige Hooks/Interaktion:
    - `PreparePlayback` setzt Samplerate-abhängige Filter.
    - `AbortPlayback` räumt Instanzen und Sampler auf.

- GOReferencePipe.{cpp}
  - REF-Pipes: parsen `REF:manual:stop:pipe`, registrieren Referenz-ID im referenzierten Pipe (via `RegisterReference`) und leiten `VelocityChanged` an die referenzierte Pipe weiter (`m_Reference->SetVelocity(velocity, m_ReferenceID)`).

- GORank.{h,cpp}
  - Sammelt `GOPipe`-Objekte (GOSoundingPipe, GOReferencePipe, GODummyPipe).
  - Beim Laden: erzeugt Pipes in `GORank::Load` (DUMMY / REF: / SOUND).
  - `GORank::SetKey` berechnet Max-Velocity über Stops und ruft `m_Pipes[note]->SetVelocity(max)`.

- GOManual.{h,cpp}
  - Tastatur- und Key-Management.
  - `GOManual::SetKey` aktualisiert m_Velocities/RemoteVelocity, ruft `PropagateKeyToCouplers` und `SetOutput`.
  - `PropagateKeyToCouplers` ruft `GOCoupler::SetKey(...)` für eingetragene Coupler.
  - `SetOutput` verteilt Werte an Stops (`GOStop::SetKey`) und sendet Division-MIDI.

- GOStop/GODrawstop.{h,cpp}
  - Drawstops (Stops) sind Schalter, die Ranks steuern.
  - `GOStop::Load` bindet Ranks, berechnet Pipe-Zuordnungen.
  - `GOStop::SetKey` / `SetRankKey` ruft `GORank::SetKey` für jeweilige Ranks auf.
  - `GODrawstop` erlaubt logische Kombinationen (AND/OR/NOT/XOR), mehrere Controlling-Drawstops, interne states und Kombination-Handling.

- GOCoupler.{h,cpp}
  - Koppler zwischen Manuals (intermanual) oder innerhalb eines Manuals (intramanual).
  - Verantwortlich für Weiterleitung von Key-States: `SetKey(... velocities ...)` entscheidet über resultierende Out-Velocity und ruft `dest->SetKey(...)`.
  - Support für special types (BASS/MELODY), UnisonOff, rekursive Verhalten.

- GOWindchest.{h,cpp}
  - Gruppiert Ranks/Pipes/Enclosures pro Windchest.
  - `AddPipe`, `AddRank`, `AddEnclosure`. `UpdateTremulant` setzt Wave-Tremulant State in allen Pipes.

- GOTremulant.{h,cpp}
  - Tremulant-Objekte (Synth oder Wave).
  - Synth: `GOSoundProviderSynthedTrem` wird erstellt; `StartPlayback`/`OnDrawstopStateChanged` ruft `GOSoundEngine::StartTremulantSample/StopSample`.
  - Wave: `r_OrganModel.UpdateTremulant(this)` informiert `GOWindchest` -> Pipes.

- GOReferencingObject.{h,cpp}
  - Helfer: Referenz-resolve hook `ResolveReferences()` (wird von GOOrganController nach Load über `GetReferencingObjects()` aufgerufen).

- GOCacheObject.{h,cpp}
  - Fehler-resistente Wrapper: `InitWithoutExc`, `LoadFromFileWithoutExc`, `LoadFromCacheWithoutExc`, `GenerateMessage`.
  - Modellobjekte (z. B. GOSoundingPipe, GOTremulant) registrieren sich bei `GOEventHandlerList` als Cache-Objekte.

2) Voice / Sampler Lifecycle (konkret)
- Auslöser: `GOPipe::SetVelocity` → `m_Rank->SendKey(midiKey, velocity)` → `GOPipe::VelocityChanged` (konkret in `GOSoundingPipe`).
- Start einer gesampelten Stimme:
  - `GOSoundingPipe::VelocityChanged` (on press) ruft `GOSoundEngine::StartPipeSample(&m_SoundProvider, m_WindchestN, m_AudioGroupID, velocity, delay, m_LastStop, is_release, &m_LastStart)` und erhält `GOSoundSampler*`.
  - Wenn Sampler vorhanden, `m_Instances++`. Nicht-Oneshot → setze `p_CurrentLoopSampler`.
- Release:
  - Bei Release (`m_Instances` > 0 und velocity == 0) → `StopSample(&m_SoundProvider, p_CurrentLoopSampler)`; `p_CurrentLoopSampler = nullptr`.
  - Bei IndependentRelease-Config → Engine startet Release-Sample (StartPipeSample with release=true).
- Velocity-Änderung während gehalten: `GOSoundEngine::UpdateVelocity(&m_SoundProvider, p_CurrentLoopSampler, velocity)`.
- Abort/Prepare:
  - `PreparePlayback` initialisiert Sampler-fähige Parameter (z. B. Filter-Samplerate).
  - `AbortPlayback` setzt `m_Instances = 0`, `p_CurrentLoopSampler = NULL`, `m_LastStop = 0` und `m_SoundProvider.SetWaveTremulant(false)`.

3) Cache & Parallel-Load-Mechanik
- Beim Laden prüft `GOOrganController::Load` ob Cache vorhanden:
  - Falls Cache valide → `GOCache` reader ruft `LoadCache` auf modell-Objekten (impl. z.B. `GOSoundingPipe::LoadCache`).
  - Falls Cache fehlt/ungültig → `GOLoadWorker` + N×`GOLoadThread` laden Objekte parallel via `LoadData`.
- `GOCacheObject` kapselt Ausnahmebehandlung beim Laden. `GOOrganController` verwendet `GOCacheObjectDistributor` zur Fortschrittsanzeige.

4) Event/Handler Integration
- Viele Objekte registrieren sich in `GOEventHandlerList` für:
  - SoundStateHandlers (PreparePlayback/Start/Abort)
  - CacheObjects (Load/Save)
  - SaveableObjects (Combinations)
  - MidiEventHandlers
- `GOEventDistributor` (Control layer) iteriert über diese Collections und ruft passende Methoden (z. B. `PreparePlayback`).

5) Wichtige Implementierungs-Hotspots (Symbole, als Sprungziele für clangd)
- Model Load / Resolve:
  - GOOrganController::Load (entrypoint) → GOOrganModel::Load
  - GOReferencingObject::ResolveReferences
- Voice lifecycle:
  - GOSoundingPipe::VelocityChanged
  - GOSoundEngine::StartPipeSample / StopSample / UpdateVelocity (in sound/)
- Cache/load:
  - GOCacheObject::LoadFromCacheWithoutExc / LoadFromFileWithoutExc
  - GOOrganController cache-handling (in controller code)
  - GOLoadWorker / GOLoadThread
- Playback orchestration:
  - GOEventDistributor::PreparePlayback / StartPlayback / AbortPlayback
  - GORank::PreparePlayback, GOManual::PreparePlayback
- Coupler/Propagation:
  - GOCoupler::SetKey / ChangeKey / SetOut
  - GOManual::PropagateKeyToCouplers, GOManual::SetKey

6) Design-Patterns & Hinweise
- Deferred resolution: `GOReferencingObject` allows creating objects that reference others and resolving references after whole model loaded.
- Registration + centralized iteration: `GOEventHandlerList` pattern reduces coupling between controller/engine and individual model objects.
- Cache-first loading with graceful fallback, plus parallel worker threads to speed up heavy IO-bound operations (samples).
- Pipes are pluggable: sampled pipes vs reference vs dummy vs synthesized trem. Provider classes encapsulate low-level sample handling.

7) Open Questions / TODO (konkrete nächste Schritte)
- Ergänze Datei:Zeile Sprünge mit clangd für:
  - GOSoundingPipe::VelocityChanged (done, file present)
  - GOSoundEngine::StartPipeSample/StopSample/UpdateVelocity (sound tree)
  - GOOrganModel::Load (already present)
  - GOEventDistributor::PreparePlayback
- Prüfe Thread-Safety / Locking im Zusammenspiel GOLoadThread vs. Engine Callback (wo m_SoundProvider Daten gelesen/gesetzt werden).
- Erweitere Map mit kurze Call-Graph SVG/ASCII, falls gewünscht (opt.: script to extract call graph via clangd).

8) Status / Nächste Aktionen
- Ich habe alle Kern-Model-Dateien gelesen und die Map (`docs/RepoMap/ModelFull.md`) ergänzt.
- Vorschlag: als nächstes kann ich
  - a) automatisch mit clangd konkrete Datei:Zeile Verweise in die Maps einpflegen (ich kann die Stellen vorschlagen, du führst "Go to definition" aus oder ich kann versuchen, die relevanten sound-Funktionen ebenfalls zu lesen),
  - b) eine kompakte "Aufgabenliste" erzeugen, was noch zu überprüfen ist (threading, edge cases, performance),
  - c) mit dem Sound-Tree (GOSoundEngine) weitermachen, um die Engine-Schnittstellen genauer zu dokumentieren.

Welche Option möchtest du als nächstes? (Kurzantwort genügt.)
