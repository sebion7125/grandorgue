#pragma once

// === EIN / AUS Schalter ===
#define ENABLE_CHAMADE_DEBUG  0   // 1 = an, 0 = aus

#if ENABLE_CHAMADE_DEBUG
  #include <wx/log.h>
  #define CHAMADE_DEBUG(...) wxLogMessage(__VA_ARGS__)
#else
  #define CHAMADE_DEBUG(...) do {} while(0)
#endif