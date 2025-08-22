# Sound-Engine – Vollanalyse

Kurz:
Diese Map fasst Architektur und zentrale Pfade der Sound-Engine zusammen (src/grandorgue/sound). Ziel: schnelle Orientierung, Hotspots zum Debugging (Voice‑Lifecycle, Scheduler, I/O, Resampling, Reverb, Fader), und Empfehlungen, welche Stellen mit clangd verlinkt werden sollten.

1) Gesamtarchitektur (High-level)
- GOSoundEngine ist die Orchestrierungs-Schicht:
  - Empfang von Sample-Start-/Stop-Anfragen (StartPipeSample / StopSample / UpdateVelocity).
  - Verwaltung von Tasks/Threads (Scheduler → GOSoundScheduler + Tasks in scheduler/).
  - Pooling/Verwaltung von Samplern (GOSoundSampler, GOSoundSamplerPool).
  - Audio-Output: GetAudioOutput / GetEmptyAudioOutput für Port-Backends.
  - Reverb/Resampling/Fading/Filter und Metering integriert (Reverb‑Engine, ToneBalance, Resample).
- SoundProvider-Objekte (GOSoundProvider*, Wave/SynthedTrem) liefern rohe Sample-Daten und Metadaten an die Engine.
- Die Engine teilt Arbeit in Tasks (Windchest, Tremulant, Group, Output, Release, Touch, etc.) – siehe scheduler/*. Die Tasks werden vom Scheduler geplant.

2) Wichtige Komponenten (Dateien)
- Engine core:
  - GOSoundEngine.h / GOSoundEngine.cpp — zentrale API & Implementierung
  - scheduler/GOSoundScheduler.* — Task-Verwaltung / Dispatch
  - scheduler/GOSoundThread.* — Worker-Threads für Audiotasks
  - scheduler/GOSound*Task.* — konkrete Tasks (Windchest, Release, Touch, Tremulant, Output, Group)
- Sampler / Pool:
  - GOSoundSampler.h — Sampler-Struktur (Handle)
  - GOSoundSamplerPool.* — Pool-Management von Samplern
  - GOSoundSamplerList / SimpleSamplerList — Sampler-Collections
- Provider / Samples:
  - GOSoundProvider.* — Abstraktion für sample- oder synth-basierte Quellen
  - GOSoundProviderWave.* — wave-file based provider (attack/release, loops)
  - GOSoundProviderSynthedTrem.* — synthetischer Tremulant
- Fader / Crossfade / Release:
  - GOSoundFader.* — Fader-Logik / Crossfade-Parameter
  - GOSoundReleaseAlignTable.* — Release-Alignment
- Effekte / Filter:
  - GOSoundReverb.* (Reverb Engine / Partition)
  - GOSoundToneBalanceFilter.* — Tonbalance-Filter
  - GOSoundResample.* — Resampling / Interpolation
- I/O / Ports:
  - ports/GOSoundPort* (PortAudio, Rt, JACK) und ports/GOSoundPortFactory.*
  - GOSoundStream.* — Stream-Glue
- Recorder:
  - GOSoundRecorder.* — optionaler Audio-Recorder

3) Kern-Flows / Call‑Chains (Essentials)
- Start eines Pipe‑Samples (aus Modell):
  - Modell → GOSoundingPipe::VelocityChanged → GOSoundEngine::StartPipeSample (inline Call) → GOSoundEngine::CreateTaskSample → Create/allocate GOSoundSampler → Scheduler/Task bekommt Sampler → später Verarbeitung durch Windchest/Group/Output Tasks → Samples in GetAudioOutput gemischt → Port-Ausgabe.
- Stop/Release:
  - Modell → GOSoundEngine::StopSample(handle) → Engine plant Release-Handling (StopSample gibt m_LastStop zurück) → ggf. detached release task oder StartReleaseSampler → ReleaseTask mischt Release zu Output bzw. berechnet Release-alignment.
- Velocity-Update:
  - GOSoundEngine::UpdateVelocity → Task/Sampler wird mit neuer Lautstärke/Param versorgt (beachtet real-time constraints).

4) Scheduler & Tasks (kurz)
- GOSoundScheduler: verwaltet Task-Queues und Zuweisungen.
- Tasks (z. B. GOSoundWindchestTask) sind zuständig für:
  - Iteration über Sampler der jeweiligen Domäne (Windchest → Pipes)
  - Mixen von Fragmenten in Zwischenpuffer
  - Delegation an Output-Task/Group-Task
- GOSoundThread führt Tasks aus und sorgt für Thread‑Isolation (real‑time considerations).

5) Echtzeit-/Threading‑Hinweise
- Engine benutzt Worker-Threads (scheduler) und atomare Zähler (m_UsedPolyphony, m_HasBeenSetup).
- Sampler-Pools vermeiden Allokationen im Audiopfad.
- Viele Operationen müssen lock‑free / reentrancy-sicher sein — kritische Stellen:
  - StartPipeSample / StopSample (können aus Modell-Thread aufgerufen werden)
  - ProcessSampler / PassSampler / ReturnSampler (Audio-Task-Kontext)
  - Zugriff auf GOSoundProvider Daten (Load/LoadCache vs. Playback) — Race-Check mit GOLoadThread nötig.

6) Hotspots / Empfohlene Sprungziele (für clangd)
- GOSoundEngine::CreateTaskSample / StartPipeSample / StopSample / UpdateVelocity
- GOSoundScheduler::(Start/Enqueue/Dispatch) + GOSoundThread::run entrypoints (scheduler/)
- GOSoundSamplerPool::{Get/Return} / GOSoundSampler structure
- GOSoundProviderWave::LoadFromMultipleFiles, GetSampleFrame (in provider)
- GOSoundFader::apply / GOCrossfadeMode usage
- GOSoundReverbEngine (initialization and per‑block processing)
- ports/GOSoundPortaudioPort.cpp (callback integration with engine)

7) Empfehlungen / nächste Schritte (konkret)
- Ich kann jetzt:
  - A) GOSoundEngine.cpp komplett lesen und die Engine‑Map präzisieren (Call‑graph + Datei:Zeile Hotspots).
  - B) Scheduler-Ordner (scheduler/*.cpp) lesen, um Task‑Mapping & threading genauer zu dokumentieren.
  - C) Provider‑Ordner (GOSoundProviderWave, GOSoundProvider.cpp) lesen, um Sample‑I/O und loop/crossfade-Mechanik zu beschreiben.
- Vorschlag: Ich lese nacheinander GOSoundEngine.cpp → scheduler → provider → ports → Reverb/Fader/Resample und aktualisiere docs/RepoMap/SoundEngine-Full.md iterativ.

8) Status / Task-Progress (aktuell)
- [x] Sound-Dateien aufgelistet
- [x] GOSoundEngine.h gelesen (API & Struktur)
- [ ] GOSoundEngine.cpp lesen
- [ ] scheduler/ Dateien lesen
- [ ] provider (Wave/SynthedTrem) lesen
- [ ] ports und I/O Callback lesen
- [ ] Reverb/Fader/Resample lesen
- [ ] Engine-Map finalisieren (docs/RepoMap/SoundEngine-Full.md)

Möchtest du, dass ich sofort GOSoundEngine.cpp lese und die Engine‑Map weiter ausarbeite? Wenn ja, bestätige kurz (oder sag "ja, bitte").
