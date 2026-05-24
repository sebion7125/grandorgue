#pragma once

// Lookup tables for attack/transient behaviour per MIDI note (C0 to G9)

/*// Dry old model
extern const float curvature_dry_by_midi[128];     // parabola-fit curvature
extern const float attack_time_dry_by_midi[128];   // attack duration in milliseconds

// Front
extern const float curvature_front_by_midi[128];   // parabola-fit curvature
extern const float attack_time_front_by_midi[128]; // attack duration in milliseconds

// Rear
extern const float curvature_rear_by_midi[128];    // parabola-fit curvature
extern const float attack_time_rear_by_midi[128];  // attack duration in milliseconds*/



// Dry
extern const float g0_dry_by_midi[128];
extern const float tmax_dry_by_midi[128];

// Front
extern const float g0_front_by_midi[128];
extern const float tmax_front_by_midi[128];

// Rear
extern const float g0_rear_by_midi[128];
extern const float tmax_rear_by_midi[128];
