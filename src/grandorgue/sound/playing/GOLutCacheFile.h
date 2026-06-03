/*
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOLUT_CACHE_FILE_H
#define GOLUT_CACHE_FILE_H

#include <cstdint>
#include <vector>

#include <wx/string.h>

// Increment when the binary file layout changes (incompatible header/section
// format).
static constexpr uint32_t GOLUT_FORMAT_VERSION = 1;

// Increment when the LUT computation algorithm changes such that previously
// cached {loop_pos, best_r} values would be incorrect for new GO builds.
static constexpr uint32_t GOLUT_ALGORITHM_VERSION = 1;

// Magic bytes at the start of every .golut file (8 bytes incl. null).
static constexpr char GOLUT_MAGIC[8] = {'G', 'O', 'R', 'A', 'L', 'C', '1', '\0'};

// One LUT support point as stored on disk.
struct GOLutPoint {
  uint32_t loop_pos; // absolute sample position in attack (n * period_samples)
  uint16_t best_r;   // best release offset r* in [0, T)
};

// One complete LUT for a single release (all attack variants share the period).
using GOLutEntry = std::vector<GOLutPoint>;

// Generator quality criteria stored in the file for diagnostics.
// These are NOT used for cache validation — only for the status dialog.
struct GOLutGeneratorCriteria {
  float    minScore            = 0.35f;
  float    minCoherence        = 0.75f;
  uint32_t maxGapFills         = 8;
  uint8_t  requireStabilization = 1;
  uint8_t  useDriftFallback    = 1;
  uint8_t  _pad[2]             = {};
};

// ── Writer ────────────────────────────────────────────────────────────────────

class GOLutCacheWriter {
public:
  // Write a .golut file atomically (temp → rename).
  // releaseMap[i] = -1 (no LUT) or index into luts[].
  // Returns true on success.
  static bool Write(
    const wxString              &path,
    const wxString              &odfHash,
    uint32_t                     releaseCount,
    const std::vector<int32_t>  &releaseMap,
    const std::vector<GOLutEntry> &luts,
    const GOLutGeneratorCriteria &criteria);

  // Derive the canonical .golut path from the GO cache directory and organ hash.
  static wxString MakePath(
    const wxString &cacheDir, const wxString &organHash);
};

// ── Reader ────────────────────────────────────────────────────────────────────

class GOLutCacheReader {
public:
  // Attempt to load a .golut file.  Returns true only if the file exists,
  // the header is valid, the odfHash and releaseCount match, and all data
  // can be read without error.  On any mismatch or read error: returns false
  // silently (caller falls back to live computation).
  bool Load(
    const wxString &path,
    const wxString &expectedOdfHash,
    uint32_t        expectedReleaseCount);

  bool                           IsValid()      const { return m_valid; }
  const std::vector<int32_t>    &GetReleaseMap() const { return m_releaseMap; }
  const std::vector<GOLutEntry> &GetLuts()       const { return m_luts; }
  const GOLutGeneratorCriteria  &GetCriteria()   const { return m_criteria; }

private:
  bool                    m_valid = false;
  std::vector<int32_t>    m_releaseMap;
  std::vector<GOLutEntry> m_luts;
  GOLutGeneratorCriteria  m_criteria;
};

#endif /* GOLUT_CACHE_FILE_H */
