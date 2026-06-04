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
  // Load a .golut file.  Returns true on success.
  // headerOnly=false (default): reads and validates everything; caller can
  //   iterate GetReleaseMap() / GetLuts() to apply LUTs.
  // headerOnly=true: reads only the fixed header (~104 bytes) and validates
  //   magic, versions, hash and counts.  GetLutCount() is populated;
  //   GetReleaseMap() / GetLuts() remain empty.  Use for fast status checks.
  bool Load(
    const wxString &path,
    const wxString &expectedOdfHash,
    uint32_t        expectedReleaseCount,
    bool            headerOnly = false);

  bool                           IsValid()      const { return m_valid; }
  uint32_t                       GetLutCount()  const { return m_lutCount; }
  const std::vector<int32_t>    &GetReleaseMap() const { return m_releaseMap; }
  const std::vector<GOLutEntry> &GetLuts()       const { return m_luts; }
  const GOLutGeneratorCriteria  &GetCriteria()   const { return m_criteria; }

private:
  bool                    m_valid    = false;
  uint32_t                m_lutCount = 0;
  std::vector<int32_t>    m_releaseMap;
  std::vector<GOLutEntry> m_luts;
  GOLutGeneratorCriteria  m_criteria;
};

#endif /* GOLUT_CACHE_FILE_H */
