#pragma once

// Lookup-Tabellen für Einschwingverhalten pro MIDI-Note (C0 bis G9)

/*// Dry altes modell
extern const float curvature_dry_by_midi[128];     // Parabelfit-Krümmung
extern const float attack_time_dry_by_midi[128];   // Attack-Zeit in Millisekunden

// Front
extern const float curvature_front_by_midi[128];   // Parabelfit-Krümmung
extern const float attack_time_front_by_midi[128]; // Attack-Zeit in Millisekunden

// Rear
extern const float curvature_rear_by_midi[128];    // Parabelfit-Krümmung
extern const float attack_time_rear_by_midi[128];  // Attack-Zeit in Millisekunden*/



// Dry
extern const float g0_dry_by_midi[128];
extern const float tmax_dry_by_midi[128];

// Front
extern const float g0_front_by_midi[128];
extern const float tmax_front_by_midi[128];

// Rear
extern const float g0_rear_by_midi[128];
extern const float tmax_rear_by_midi[128];
