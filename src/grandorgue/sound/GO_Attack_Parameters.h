#pragma once

// Lookup-Tabellen für Einschwingverhalten pro MIDI-Note (C0 bis G9)

// Dry
extern const float curvature_dry_by_midi[128];     // Parabelfit-Krümmung
extern const float attack_time_dry_by_midi[128];   // Attack-Zeit in Millisekunden

// Front
extern const float curvature_front_by_midi[128];   // Parabelfit-Krümmung
extern const float attack_time_front_by_midi[128]; // Attack-Zeit in Millisekunden

// Rear
extern const float curvature_rear_by_midi[128];    // Parabelfit-Krümmung
extern const float attack_time_rear_by_midi[128];  // Attack-Zeit in Millisekunden