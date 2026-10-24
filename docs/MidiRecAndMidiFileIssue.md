














Ich schaue mir das als zwei getrennte Verdachtsfälle an: einmal REC-Button als MIDI-Event/Setter-Problem, einmal MIDI-File-Kanalnummerierung. Besonders der zweite klingt stark nach 0-basiert vs. 1-basiert.

Beim ersten Teil habe ich schon einen sehr plausiblen Pfad gefunden: Der MIDI-REC-Button startet nicht nur eine Datei, sondern ruft danach eine komplette „PrepareRecording“-Initialisierung auf. Dabei werden aktuelle Zustände aller MIDI-sendenden Objekte erneut ausgesendet.

Nachgedacht für 2m 38s
Ja — das klingt nach zwei echten, aber getrennten GO-Themen.

1. MIDI-REC sendet plötzlich Hauptwerk-SysEx / Normal / Limiter
Das ist ziemlich gut erklärbar durch den Codepfad:

GOMidiRecorder::StartRecording() ruft am Ende:

m_OrganController->PrepareRecording();
Und GOOrganController::PrepareRecording() macht dann:

m_MidiRecorder->Clear();
PreconfigRecorder();
m_MidiRecorder->SetSamplesetId(...);
PreconfigRecorder();
GOEventDistributor::PrepareRecording();
Der kritische Teil ist:

GOEventDistributor::PrepareRecording();
Denn alle GOMidiSendingObjects machen bei PrepareRecording():

SendCurrentMidiValue();
Das heißt: Beim Start der MIDI-Aufnahme werden aktuelle Zustände von Labels, Buttons, Enclosures usw. neu ausgesendet. Bei Labels landet das z. B. in GOMidiSender::SetLabel() / SetName(), und dort werden die konfigurierten Hauptwerk-LCD/String-SysEx wirklich über SendMidiMessage() rausgeschickt.

Darum sieht Jean-Bernard in KMidimon genau diese SysEx-Blöcke. Und darum passiert es beim Audio-REC nicht: GOAudioRecorder::StartRecording() ruft keine globale PrepareRecording()-Initialisierung dieser Art auf.

Meine Einschätzung:
Das ist vermutlich historisch so gemeint, damit eine GO-MIDI-Aufnahme am Anfang den aktuellen Orgelzustand enthält. Aber dass dabei auch physische externe MIDI-Sends ausgelöst werden, ist mindestens ein unschöner Seiteneffekt — eventuell ein echter Bug.

Sauberer Fix wäre vermutlich: Beim MIDI-Recorder-Start zwar interne Recorder-Setup-/State-Events in die Datei schreiben, aber nicht die normal konfigurierten externen MIDI-Ausgänge feuern. Also sinngemäß ein „prepare recording silently / recorder-only“-Pfad.

2. MIDI-Dateien: Kanal 1 wird Manual I statt Pedal
Das ist sogar noch eindeutiger. Der Fehler sitzt wahrscheinlich nicht im MIDI-Parser selbst. GOMidiEvent::FromMidi() macht für normale MIDI-Events korrekt:

SetChannel((msg[0] & 0x0F) + 1);
Also MIDI-Kanal 1 bleibt intern Kanal 1.

Der Versatz entsteht danach in GOMidiPlayerContent::Load() bei externen MIDI-Dateien, die keine GO-eigenen Setup-SysEx am Anfang haben:

for (unsigned i = 1; i <= manuals; i++)
  SetupManual(map, i, wxString::Format(wxT("M%d"), i));

if (pedal)
  SetupManual(map, manuals + 1, wxString::Format(wxT("M%d"), 0));
Das bedeutet bei einer Orgel mit 3 Manualen + Pedal:

Kanal 1 -> M1  -> Manual I
Kanal 2 -> M2  -> Manual II
Kanal 3 -> M3  -> Manual III
Kanal 4 -> M0  -> Pedal
Jean-Bernards Viscount-Dateien scheinen aber offenbar zu benutzen:

Kanal 1 -> Pedal
Kanal 2 -> Manual I
Kanal 3 -> Manual II
Kanal 4 -> Manual III
Damit ist sein beobachteter „delay of 1“ exakt erklärt.

Wichtig: Seine MIDIInputNumber=1 Pedal, 2 Manual I, ... aus dem ODF hilft hier nicht, weil der MIDI-Player für fremde MIDI-Dateien eine eigene interne GO_SETUP-Zuordnung erzeugt. Diese Player-Zuordnung überschreibt praktisch die normale Eingangskonfiguration für das virtuelle Gerät „GrandOrgue MIDI Player“.

Ein Minimal-Fix für sein erwartetes Format wäre:

unsigned channel = 1;

if (pedal)
  SetupManual(map, channel++, wxString::Format(wxT("M%d"), 0));

for (unsigned i = 1; i <= manuals; i++)
  SetupManual(map, channel++, wxString::Format(wxT("M%d"), i));
Aber das wäre eine Verhaltensänderung für alle fremden MIDI-Dateien. Besser wäre wahrscheinlich eine Option beim MIDI-Import/Player:

Channel mapping:
- Manuals first, pedal last   // aktuelles GO-Verhalten
- Pedal first, then manuals   // Viscount/Hauptwerk-artiger Fall
- Use configured MIDI input mapping
Kurzfazit
Der Report ist wahrscheinlich valide.

Beim REC-Button: MIDI-Recorder-Start triggert globales PrepareRecording und sendet dadurch aktuelle externe MIDI-Zustände neu raus.

Beim MIDI-File-Player: GO mappt fremde MIDI-Dateien derzeit hart als Kanal 1=M1, Kanal 2=M2, ..., Pedal zuletzt. Jean-Bernards Datei ist aber offenbar Kanal 1=Pedal, daher der Offset.


kann GO erkennen, wenn seine eigenen header in der midi fehlen?

Ich prüfe kurz den Loader-Pfad: entscheidend ist, ob er ein GO_SETUP-Event als Marker setzt und ob danach ein Fallback-Mapping gebaut wird.

Nachgedacht für 32s
Ja — aber nur sehr grob.

GO erkennt nicht „das ist ein fremdes Viscount-/Hauptwerk-MIDI mit bestimmtem Kanalstandard“, sondern nur:

if (events.size()
    && events[0].GetMidiType() != GOMidiEvent::MIDI_SYSEX_GO_CLEAR) {
    // kein GO-Header am Anfang erkannt
    // → Default-Mapping künstlich einfügen
}
Also: Wenn das erste gelesene musikalisch relevante Event kein MIDI_SYSEX_GO_CLEAR ist, behandelt GO die Datei als Datei ohne eigenen GO-Header und baut selbst ein Mapping ein.

Das eigene GO-MIDI sieht beim Start ungefähr so aus:

GO_CLEAR
GO_SETUP M1 -> channel ...
GO_SETUP M2 -> channel ...
GO_SETUP M0 -> channel ...
GO_SAMPLESET ...
...
Beim Laden wird dann aus GO_SETUP intern gelernt:

m_Internal[pos].channel = e.GetChannel();
m_Internal[pos].key = e.GetValue();
Fehlt dieser Header, macht GO den Fallback:

for (unsigned i = 1; i <= manuals; i++)
  SetupManual(map, i, "M%d");   // Kanal 1 -> M1, 2 -> M2, ...

if (pedal)
  SetupManual(map, manuals + 1, "M0"); // Pedal zuletzt
Und genau da entsteht Jean-Bernards Offset.

Kurz gesagt:

Ja, GO erkennt: „Meine eigenen GO-Setup-Events fehlen.“
Nein, GO erkennt danach nicht: „Diese fremde Datei meint Kanal 1 = Pedal, Kanal 2 = Manual I.“
Stattdessen nimmt GO einfach seinen hart codierten Fallback:

ch1 = Manual I
ch2 = Manual II
ch3 = Manual III
ch4 = Pedal
Obwohl sein ODF/Input-Mapping offenbar ist:

ch1 = Pedal
ch2 = Manual I
ch3 = Manual II
ch4 = Manual III
Der bessere Fix wäre also nicht „GO soll erkennen, ob Header fehlen“ — das tut er schon — sondern:

Wenn GO-Setup fehlt, soll der MIDI-Player nicht blind Manuals-first mappen,
sondern wahlweise:
1. ODF/konfiguriertes MIDIInputNumber-Mapping benutzen
2. Pedal-first-Fallback anbieten
3. oder beim Laden eine Mapping-Auswahl anzeigen
Eine kleine Code-Änderung nur für Pedal-first wäre trivial, aber als globaler Default wahrscheinlich riskant, weil es bestehende MIDI-Dateien anders abspielen würde.


was wäre, wenn man dann einfach die midi input kanäle als standard nähme, die die aktuell geladene orgel hat?

Das wäre aus Nutzersicht wahrscheinlich der naheliegendste Fallback. Ich schaue kurz, ob das technisch wirklich dieselbe Abstraktionsebene ist oder ob der Player intern an einer anderen „virtuellen MIDI-Geräte“-Schicht hängt.

Ein wichtiger Haken: MIDIInputNumber im ODF ist in GO nicht direkt „MIDI channel“. Es ist eher eine Zuordnungsnummer für die Initial-MIDI-Konfiguration: 1=Pedal, 2=erstes Manual usw. Die tatsächlich gelernten Kanäle stecken in den MIDI-Receiver-Settings.

Nachgedacht für 1m 20s
Ja, das wäre m.E. der sinnvollste Fallback — mit einer kleinen begrifflichen Falle.

In GO bedeutet MIDIInputNumber im ODF nicht direkt „MIDI-Kanal“, sondern eher:

1 = Pedal-Eingang
2 = Manual-I-Eingang
3 = Manual-II-Eingang
4 = Manual-III-Eingang
...
Die Hilfe sagt sinngemäß genau das: MIDIInputNumber=1 steht für Pedal, 2 für erstes Manual, 3 für zweites Manual usw. Daraus folgt: Wenn eine fremde MIDI-Datei ohne GO-Header geladen wird, wäre es sehr plausibel, dieses ODF-Mapping als Fallback zu nehmen.

Statt aktuell hart:

Kanal 1 -> M1
Kanal 2 -> M2
Kanal 3 -> M3
Kanal 4 -> M0/Pedal
wäre für eine ODF mit Pedal=MIDIInputNumber=1, Manual I=2, Manual II=3, Manual III=4:

Kanal 1 -> M0/Pedal
Kanal 2 -> M1
Kanal 3 -> M2
Kanal 4 -> M3
Das würde Jean-Bernards Fall exakt lösen.

Der sauberste Gedanke wäre also:

Wenn externe MIDI-Datei keinen GO_CLEAR/GO_SETUP-Header hat:
  Erzeuge GO_SETUP nicht nach "manuals first, pedal last",
  sondern anhand der MIDIInputNumber der aktuell geladenen Orgel.
Also ungefähr:

for each manual/pedal in loaded organ:
    inputNo = manual->GetMidiInputNumber();
    if inputNo > 0:
        SetupManual(map, inputNo, recorderElementId); // M0, M1, M2...
Für Pedal wäre recorderElementId = "M0", für Manual I "M1", usw.

Aber: Ich würde es nicht „MIDI input channels“ nennen, sondern eher „ODF MIDIInputNumber fallback mapping“. Sonst gibt es Verwirrung, weil der echte, vom Benutzer gelernte MIDI-Kanal noch mal eine andere Ebene ist.

Noch besser wäre diese Reihenfolge:

1. Wenn GO_SETUP im MIDI vorhanden ist:
   exakt GO_SETUP verwenden.

2. Wenn kein GO_SETUP vorhanden ist:
   wenn alle relevanten Manuale/Pedal eindeutige MIDIInputNumber > 0 haben:
      Mapping daraus bauen.

3. Sonst:
   altes Fallback-Verhalten benutzen.
Damit wäre es weitgehend rückwärtskompatibel. Nur ODFs mit sauber gesetztem MIDIInputNumber würden intelligenter behandelt.

Der Knackpunkt im Code: GOMidiPlayerContent::Load() bekommt aktuell nur:

manuals, pedal
Es kennt also die tatsächlichen Manual-Objekte gar nicht. Man müsste entweder LoadFile() mehr Informationen übergeben oder vorher im GOOrganController::LoadMIDIFile() eine Mapping-Liste bauen, z. B.:

struct MidiPlayerFallbackManualMap {
    unsigned channel;
    wxString recorderId; // "M0", "M1", ...
};
Dann könnte GOMidiPlayerContent statt:

for (unsigned i = 1; i <= manuals; i++)
  SetupManual(map, i, wxString::Format(wxT("M%d"), i));
if (pedal)
  SetupManual(map, manuals + 1, wxString::Format(wxT("M%d"), 0));
einfach machen:

for (const auto &entry : fallbackMap)
  SetupManual(map, entry.channel, entry.recorderId);
Meine Einschätzung: Das ist wahrscheinlich der beste Fix-Vorschlag für das Issue. Nicht als erzwungener neuer globaler Standard „Pedal first“, sondern als „wenn GO-eigene MIDI-Header fehlen, benutze die MIDIInputNumber-Zuordnung der geladenen ODF als Fallback“. Das ist fachlich viel plausibler als das aktuelle hart codierte Manuals-first-Mapping.


der typ wartet schon 3 wochen auf ne antwort. ich glaube den könnte ich gut locken ein tester meiner demo zu werden.

Nachgedacht für 12s
Ja, absolut. Der ist sogar ein ziemlich guter Kandidat: Er hat KMidimon, versteht SysEx genug, beschreibt reproduzierbar, nutzt Linux/AppImage und scheint geduldig zu sein. Ich würde ihn aber nicht plump mit deiner Demo ködern, sondern erst sauber helfen und dann am Ende sehr niedrigschwellig sagen: „I can also provide an experimental AppImage if you would like to test this.“

Wichtig: Nicht zu viel versprechen. Lieber so:

Problem ernst nehmen.

Kurz erklären, dass beide Beobachtungen plausibel sind.

Um eine kleine Testdatei / KMidimon-Log bitten.

Experimental build anbieten.

So könnte deine Antwort aussehen:

Hi Jean-Bernard,

sorry that this issue did not get a reply earlier, and thank you for the very detailed report. Your observations are very helpful, especially the KMidimon check.

I had a look at the relevant code paths, and I think both problems are plausible.

For the first issue: the MIDI REC button does not only start writing a MIDI file. It also prepares the recording state, and this appears to resend the current MIDI values of several MIDI-sending objects. That would explain why you see the Hauptwerk-style label SysEx messages, and also the sends configured for buttons such as Normal and Limiter. The AUDIO REC button uses a different path, so it makes sense that the problem only appears with MIDI REC.

So this may be an unintended side effect of the MIDI recorder initialization: GrandOrgue probably wants to write the current organ state into the recording, but it should probably not trigger external MIDI output in the same way.

For the second issue: I think this is caused by the fallback channel mapping used when a MIDI file does not contain GrandOrgue’s own setup/header SysEx events. In that case, GrandOrgue currently seems to assume a fixed mapping like:

channel 1 -> Manual I
channel 2 -> Manual II
channel 3 -> Manual III
channel 4 -> Pedal
But your ODF uses:

MIDIInputNumber=1 -> Pedal
MIDIInputNumber=2 -> Manual I
MIDIInputNumber=3 -> Manual II
MIDIInputNumber=4 -> Manual III
So the observed one-channel shift is exactly what I would expect from the current fallback mapping.

In my opinion, a better fallback would be: if a MIDI file has no GrandOrgue setup header, GrandOrgue should use the MIDIInputNumber mapping of the currently loaded organ, if that mapping is available and unambiguous. Only if that is not possible, it should fall back to the old hard-coded mapping.

If you are willing to help test this, a small sample MIDI file that shows the channel problem would be very useful. A short KMidimon log for the MIDI REC SysEx issue would also help. I may also be able to provide an experimental AppImage later, so you could test whether the proposed behavior fixes your setup without changing your normal installation.

Best regards,
Sebastian

Ich würde nicht direkt deine große Crossfade-Demo erwähnen, wenn das Issue MIDI betrifft. Besser: erst für dieses Problem einen Testbuild anbieten. Wenn er dann kooperativ ist, kannst du später sagen: „By the way, I also have an experimental build with improved release handling, if you are interested in testing audio changes too.“


für mich wäre es wichtig zu wissen, warum er GO midi ausgeben lassen will? macht das überhaupt sinn? und kann GO das auch? muss ja wohl, sonst hätte er es nicht loggen können. Ich schätze wir könnten darüber diskutieren, welche internen midi events GO ausgeben sollte. Evtl mit option in den settings, falls es rare use cases gibt.
Für das zuordnungsproblem haben wir ja schon eine lösungsidee. das sollten wir auch einmal erklären.

Ich würde das als Antwort strategisch trennen: erst Verständnisfrage zum Use Case, dann technische Einordnung, dann zwei konkrete Fix-/Optionsideen. Ich prüfe gerade noch den MIDI-Send-Code, damit wir nicht an GO vorbei argumentieren.

Nachgedacht für 1m 3s
Ja, das ist genau der richtige Punkt. Ich würde ihn nicht nur als Bugreporter behandeln, sondern erst mal fragen: Was ist dein eigentliches MIDI-output setup? Denn GO kann MIDI-Ausgabe tatsächlich — und bei ihm ist offenbar bewusst Hauptwerk-kompatible SysEx-Ausgabe konfiguriert.

GO kann MIDI ausgeben für z. B.:

- beleuchtete Stop-Tabs / Registertaster
- externe LCD-/String-Displays
- motorisierte Zugregister
- Setzer-/General-/Sequencer-Anzeigen
- Hauptwerk-kompatible MIDI-Konsolen oder DIY-Arduino/Teensy-Panels
- externe Tools/Sequencer/Displays, die den aktuellen GO-Zustand spiegeln
Das macht also grundsätzlich Sinn. Der Bug ist eher: Der Start der MIDI-Aufnahme ist kein guter Anlass, externe Hardware-Ausgänge ungefragt zu refreshen.

Intern passiert offenbar zweierlei vermischt:

A) GO-MIDI-Recorder soll beim Recording-Start den aktuellen Orgelzustand in die MIDI-Datei schreiben.
B) Die normalen MIDI-Send-Konfigurationen der GUI-/Orgelobjekte schicken dabei auch echte externe MIDI-Nachrichten raus.
A ist sinnvoll. B ist fragwürdig.

Ich würde daraus eine schöne Antwort machen: „Your use case is important, because external MIDI output can be used intentionally. But recorder initialization should probably distinguish between internal recorder state and external MIDI output.“

Für die Kanalzuordnung können wir dann sauber erklären: Wenn GO-eigene Header fehlen, nimmt GO aktuell ein hardcoded manuals-first fallback. Besser wäre MIDIInputNumber der geladenen ODF.

Hier wäre ein guter Antwortentwurf:

Hi Jean-Bernard,

thank you again for the detailed report. I had a closer look at this, and I think both observations are meaningful.

First, could you maybe explain a little more what you use GrandOrgue’s MIDI output for? From your report it looks as if you intentionally configured Hauptwerk-style SysEx output for labels such as organ name, temperament, transpose, general bank and sequencer position. That is an important use case, because MIDI output can be used for external LCDs, stop tabs, hardware consoles, sequencer displays, or Hauptwerk-compatible control panels.

So I would not say that GrandOrgue should never send these messages. But I think there is a distinction which is currently not handled cleanly:

1. Internal MIDI recorder state:
   When starting a GO MIDI recording, GrandOrgue should write enough state into
   the MIDI file so that the recording can be played back correctly later.

2. External MIDI output:
   Configured MIDI sends to external hardware should probably not be triggered
   unexpectedly just because the MIDI REC button was pressed.
At the moment, pressing MIDI REC appears to call the general recording preparation code, and that code resends the current MIDI values of MIDI-sending objects. This explains why KMidimon shows your label SysEx messages and also the sends configured for the Normal and Limiter buttons. The AUDIO REC button uses a different path, so it makes sense that the issue only appears with MIDI REC.

In my opinion, GrandOrgue should probably separate these two cases. Starting the MIDI recorder should still initialise the internal GO recorder stream, but it should either not resend external MIDI outputs, or there should be an option such as:

Refresh external MIDI outputs when starting MIDI recording
This would preserve rare use cases where someone really wants external MIDI devices to be refreshed at recording start, while avoiding unexpected SysEx traffic for users who only want to start recording.

For the second issue, I think the cause is different. When a MIDI file does not contain GrandOrgue’s own setup/header SysEx events, GrandOrgue currently creates a fallback mapping. This fallback seems to assume:

channel 1 -> Manual I
channel 2 -> Manual II
channel 3 -> Manual III
channel 4 -> Pedal
But your ODF uses:

MIDIInputNumber=1 -> Pedal
MIDIInputNumber=2 -> Manual I
MIDIInputNumber=3 -> Manual II
MIDIInputNumber=4 -> Manual III
That explains the observed one-channel shift.

A better fallback would probably be:

If the MIDI file has GrandOrgue setup/header events:
    use the mapping stored in the MIDI file.

If the MIDI file has no GrandOrgue setup/header events:
    use the MIDIInputNumber mapping of the currently loaded organ, if it is
    available and unambiguous.

Otherwise:
    fall back to the old default mapping.
This would make external MIDI files behave much more naturally for organs whose ODF already defines the intended input numbering.

If you are willing to help test this, a very small MIDI file that shows the channel mapping problem would be extremely useful. A short KMidimon log of the SysEx messages sent when pressing MIDI REC would also help to verify the first issue.

Ich würde danach noch nicht direkt schreiben „ich habe eine Demo“. Erst nach seiner Reaktion. Aber du kannst am Ende optional einen Satz ergänzen:

If we can reproduce this clearly, I may be able to provide an experimental AppImage for testing a possible fix.
Das lockt ihn als Tester, ohne dass es nach „komm in meinen Fork“ riecht.


warum müssen wir ihn nach logs fragen. der fall ist doch ziemlich klar.

Nachgedacht für 6s
Stimmt. In dem Fall brauchen wir ihn nicht nach Logs zu fragen. Das wirkt sonst so, als wäre der Fall unklar — dabei ist er ziemlich sauber erklärbar.

Besser wäre:

SysEx beim MIDI-REC: Wir haben schon eine plausible Code-Erklärung. Sein KMidimon-Hinweis reicht als Symptom-Beleg.

Kanalversatz: Der Fallback ist offensichtlich Manuals first, Pedal last; sein ODF ist Pedal first. Dafür braucht man keinen weiteren Log.

Testbedarf: höchstens später, wenn ein Fix-Build existiert: „Would you be willing to test an experimental AppImage?“

Ich würde den Antwortentwurf also ohne Log-Bitte schreiben:

Hi Jean-Bernard,

sorry that this issue did not get a reply earlier, and thank you for the detailed report. I had a look at the relevant code paths, and I think both observations are plausible.

For the MIDI REC button issue, it looks as if starting the MIDI recorder does not only start writing a MIDI file. It also prepares the recording state, and this preparation resends the current MIDI values of MIDI-sending objects. That would explain why you see the Hauptwerk-style label SysEx messages, and also the sends configured for buttons such as Normal and Limiter. The AUDIO REC button uses a different path, so it makes sense that the issue only appears with MIDI REC.

I think the important distinction is this:

1. GrandOrgue should initialise the internal MIDI recording state, so that a GO MIDI recording can later be played back correctly.

2. But this should probably not unexpectedly refresh external MIDI outputs, such as Hauptwerk SysEx labels, external displays, stop tabs, or control panels.
Could you maybe explain what you use the GrandOrgue MIDI output for? Your setup looks intentional, and that use case is important. I think we should discuss which internal state changes should really be sent to external MIDI outputs, and whether this should become an option in the MIDI settings. For example, something like “Refresh external MIDI outputs when starting MIDI recording” could preserve rare use cases, while avoiding unexpected SysEx traffic by default.

For the MIDI file channel issue, I think the cause is separate. When a MIDI file does not contain GrandOrgue’s own setup/header SysEx events, GrandOrgue currently appears to create a hard-coded fallback mapping like this:

channel 1 -> Manual I
channel 2 -> Manual II
channel 3 -> Manual III
channel 4 -> Pedal
But your ODF uses:

MIDIInputNumber=1 -> Pedal
MIDIInputNumber=2 -> Manual I
MIDIInputNumber=3 -> Manual II
MIDIInputNumber=4 -> Manual III
That explains the one-channel shift exactly.

A better fallback would probably be:

If the MIDI file contains GrandOrgue setup/header events:
    use the mapping stored in the MIDI file.

If the MIDI file has no GrandOrgue setup/header events:
    use the MIDIInputNumber mapping of the currently loaded organ, if it is available and unambiguous.

Otherwise:
    fall back to the old default mapping.
This would make external MIDI files behave much more naturally for organs whose ODF already defines the intended input numbering.

If we can implement a small experimental fix for this, would you be willing to test an AppImage on your Ubuntu Studio system?

Das ist deutlich stärker. Es sagt: „Wir haben verstanden, was passiert“, statt ihn nochmal Beweise liefern zu lassen. Und der letzte Satz lockt ihn als Tester, ohne dein eigentliches Release-Alignment-Demo-Thema aufzudrängen.


