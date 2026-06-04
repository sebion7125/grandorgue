/*
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOLutCacheFile.h"

#include <cstring>

#include <wx/file.h>
#include <wx/filename.h>

// Maximum odfHash string length stored in the binary header (incl. null).
static constexpr size_t GOLUT_HASH_FIELD = 64;

// ── Low-level helpers ─────────────────────────────────────────────────────────

static bool WriteAll(wxFile &f, const void *data, size_t len) {
  return f.Write(data, len) == len;
}

static bool ReadAll(wxFile &f, void *data, size_t len) {
  const wxFileOffset got = f.Read(data, len);
  return got >= 0 && (size_t)got == len;
}

// ── Writer ────────────────────────────────────────────────────────────────────

wxString GOLutCacheWriter::MakePath(
  const wxString &cacheDir, const wxString &organHash) {
  return cacheDir + wxFileName::GetPathSeparator() + organHash
         + wxT(".release-align.golut");
}

bool GOLutCacheWriter::Write(
  const wxString              &path,
  const wxString              &odfHash,
  uint32_t                     releaseCount,
  const std::vector<int32_t>  &releaseMap,
  const std::vector<GOLutEntry> &luts,
  const GOLutGeneratorCriteria &criteria) {

  const wxString tmpPath = path + wxT(".tmp");

  wxFile f;
  if (!f.Open(tmpPath, wxFile::write))
    return false;

  // Helper that closes+removes the tmp file on failure.
  auto fail = [&]() -> bool {
    f.Close();
    wxRemoveFile(tmpPath);
    return false;
  };

  // ── Header ──────────────────────────────────────────────────────────────────
  if (!WriteAll(f, GOLUT_MAGIC, sizeof(GOLUT_MAGIC))) return fail();

  const uint32_t fmtVer  = GOLUT_FORMAT_VERSION;
  const uint32_t algVer  = GOLUT_ALGORITHM_VERSION;
  const uint32_t lutCount = (uint32_t)luts.size();

  if (!WriteAll(f, &fmtVer,       sizeof(fmtVer)))      return fail();
  if (!WriteAll(f, &algVer,       sizeof(algVer)))       return fail();

  // odfHash: fixed 64-byte null-terminated UTF-8 field
  char hashField[GOLUT_HASH_FIELD] = {};
  const wxScopedCharBuffer utf8 = odfHash.utf8_str();
  if (utf8.length() >= GOLUT_HASH_FIELD) return fail(); // hash too long
  strncpy(hashField, utf8.data(), GOLUT_HASH_FIELD - 1);
  if (!WriteAll(f, hashField, GOLUT_HASH_FIELD))         return fail();

  // Validate releaseMap size before writing to avoid out-of-bounds read.
  if (releaseMap.size() != releaseCount) return fail();

  if (!WriteAll(f, &releaseCount, sizeof(releaseCount))) return fail();
  if (!WriteAll(f, &lutCount,     sizeof(lutCount)))     return fail();

  // Generator criteria
  if (!WriteAll(f, &criteria.minScore,             sizeof(criteria.minScore)))             return fail();
  if (!WriteAll(f, &criteria.minCoherence,         sizeof(criteria.minCoherence)))         return fail();
  if (!WriteAll(f, &criteria.maxGapFills,          sizeof(criteria.maxGapFills)))          return fail();
  if (!WriteAll(f, &criteria.requireStabilization, sizeof(criteria.requireStabilization))) return fail();
  if (!WriteAll(f, &criteria.useDriftFallback,     sizeof(criteria.useDriftFallback)))     return fail();
  if (!WriteAll(f, criteria._pad, sizeof(criteria._pad))) return fail();

  // ── ReleaseMap ──────────────────────────────────────────────────────────────
  if (releaseCount > 0)
    if (!WriteAll(f, releaseMap.data(), releaseCount * sizeof(int32_t)))
      return fail();

  // ── LUT entries ─────────────────────────────────────────────────────────────
  for (const GOLutEntry &entry : luts) {
    const uint32_t n = (uint32_t)entry.size();
    if (!WriteAll(f, &n, sizeof(n))) return fail();
    for (const GOLutPoint &pt : entry) {
      if (!WriteAll(f, &pt.loop_pos, sizeof(pt.loop_pos))) return fail();
      if (!WriteAll(f, &pt.best_r,   sizeof(pt.best_r)))   return fail();
    }
  }

  f.Close();

  // Atomic replace: on POSIX wxRenameFile(overwrite=true) calls rename(2)
  // which replaces the destination atomically — the old cache is never
  // absent.  On Windows this is best-effort (delete+rename, not atomic).
  if (!wxRenameFile(tmpPath, path, true)) {
    wxRemoveFile(tmpPath);
    return false;
  }
  return true;
}

// ── Reader ────────────────────────────────────────────────────────────────────

bool GOLutCacheReader::Load(
  const wxString &path,
  const wxString &expectedOdfHash,
  uint32_t        expectedReleaseCount) {
  m_valid = false;
  m_releaseMap.clear();
  m_luts.clear();

  if (!wxFileExists(path))
    return false;

  wxFile f;
  if (!f.Open(path, wxFile::read))
    return false;

  // ── Header ──────────────────────────────────────────────────────────────────
  char magic[8] = {};
  if (!ReadAll(f, magic, sizeof(magic))) return false;
  if (memcmp(magic, GOLUT_MAGIC, sizeof(GOLUT_MAGIC)) != 0) return false;

  uint32_t fmtVer = 0, algVer = 0;
  if (!ReadAll(f, &fmtVer, sizeof(fmtVer))) return false;
  if (!ReadAll(f, &algVer, sizeof(algVer))) return false;
  if (fmtVer != GOLUT_FORMAT_VERSION)   return false;
  if (algVer != GOLUT_ALGORITHM_VERSION) return false;

  char hashField[GOLUT_HASH_FIELD] = {};
  if (!ReadAll(f, hashField, GOLUT_HASH_FIELD)) return false;
  hashField[GOLUT_HASH_FIELD - 1] = '\0';
  if (wxString::FromUTF8(hashField) != expectedOdfHash) return false;

  uint32_t releaseCount = 0, lutCount = 0;
  if (!ReadAll(f, &releaseCount, sizeof(releaseCount))) return false;
  if (!ReadAll(f, &lutCount,     sizeof(lutCount)))     return false;
  if (releaseCount != expectedReleaseCount) return false;
  // lutCount can't exceed the number of releases; also guard against corrupt
  // files that would cause huge allocations.
  if (lutCount > releaseCount) return false;

  // Generator criteria (read but not validated — diagnostics only)
  if (!ReadAll(f, &m_criteria.minScore,             sizeof(m_criteria.minScore)))             return false;
  if (!ReadAll(f, &m_criteria.minCoherence,         sizeof(m_criteria.minCoherence)))         return false;
  if (!ReadAll(f, &m_criteria.maxGapFills,          sizeof(m_criteria.maxGapFills)))          return false;
  if (!ReadAll(f, &m_criteria.requireStabilization, sizeof(m_criteria.requireStabilization))) return false;
  if (!ReadAll(f, &m_criteria.useDriftFallback,     sizeof(m_criteria.useDriftFallback)))     return false;
  if (!ReadAll(f, m_criteria._pad, sizeof(m_criteria._pad))) return false;

  // ── ReleaseMap ──────────────────────────────────────────────────────────────
  m_releaseMap.resize(releaseCount, -1);
  if (releaseCount > 0)
    if (!ReadAll(f, m_releaseMap.data(), releaseCount * sizeof(int32_t)))
      return false;

  // Validate: all indices must be -1 or in [0, lutCount).
  for (int32_t idx : m_releaseMap)
    if (idx != -1 && (uint32_t)idx >= lutCount) return false;

  // ── LUT entries ─────────────────────────────────────────────────────────────
  // MAX_TOTAL in ComputeCorrelationLut is 30; allow 256 for future growth.
  static constexpr uint32_t GOLUT_MAX_POINTS = 256;
  m_luts.resize(lutCount);
  for (GOLutEntry &entry : m_luts) {
    uint32_t n = 0;
    if (!ReadAll(f, &n, sizeof(n))) return false;
    if (n > GOLUT_MAX_POINTS) return false; // guard against corrupt files
    entry.resize(n);
    for (GOLutPoint &pt : entry) {
      if (!ReadAll(f, &pt.loop_pos, sizeof(pt.loop_pos))) return false;
      if (!ReadAll(f, &pt.best_r,   sizeof(pt.best_r)))   return false;
    }
  }

  m_valid = true;
  return true;
}
