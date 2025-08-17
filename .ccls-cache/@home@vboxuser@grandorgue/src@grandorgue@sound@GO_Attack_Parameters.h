#pragma once

// Lookup-Tabellen für Einschwingverhalten pro MIDI-Note (C0 bis G9)

extern const float curvature_by_midi[128];     // Parabelfit-Krümmung
extern const float attack_time_by_midi[128];   // Attack-Zeit in Millisekunden
