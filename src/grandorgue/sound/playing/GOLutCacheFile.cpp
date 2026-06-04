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

  if (releaseMap.size() != releaseCount) return false;

  // ── Serialize entirely into memory, then write in one call ────────────────
  // The previous per-field WriteAll() approach made millions of syscalls for
  // large caches.  Serialise into a buffer and write once.
  const uint32_t lutCount = (uint32_t)luts.size();

  char hashField[GOLUT_HASH_FIELD] = {};
  const wxScopedCharBuffer utf8 = odfHash.utf8_str();
  if (utf8.length() >= GOLUT_HASH_FIELD) return false;
  strncpy(hashField, utf8.data(), GOLUT_HASH_FIELD - 1);

  // Pre-calculate buffer size to avoid reallocations.
  size_t bufSize = sizeof(GOLUT_MAGIC)
    + 4 + 4 + GOLUT_HASH_FIELD + 4 + 4  // versions + hash + counts
    + sizeof(criteria.minScore) + sizeof(criteria.minCoherence)
    + sizeof(criteria.maxGapFills) + sizeof(criteria.requireStabilization)
    + sizeof(criteria.useDriftFallback) + sizeof(criteria._pad)
    + (size_t)releaseCount * sizeof(int32_t);
  for (const auto &e : luts)
    bufSize += sizeof(uint32_t) + sizeof(double)   // period_samples + period_float
              + sizeof(uint32_t)                   // n_points
              + e.points.size() * (sizeof(uint32_t) + sizeof(uint16_t));

  std::vector<uint8_t> buf;
  buf.reserve(bufSize);

  auto put = [&](const void *src, size_t n) {
    const auto *p = static_cast<const uint8_t *>(src);
    buf.insert(buf.end(), p, p + n);
  };
  auto putU32 = [&](uint32_t v) { put(&v, 4); };

  put(GOLUT_MAGIC, sizeof(GOLUT_MAGIC));
  putU32(GOLUT_FORMAT_VERSION);
  putU32(GOLUT_ALGORITHM_VERSION);
  put(hashField, GOLUT_HASH_FIELD);
  putU32(releaseCount);
  putU32(lutCount);
  put(&criteria.minScore,             sizeof(criteria.minScore));
  put(&criteria.minCoherence,         sizeof(criteria.minCoherence));
  put(&criteria.maxGapFills,          sizeof(criteria.maxGapFills));
  put(&criteria.requireStabilization, sizeof(criteria.requireStabilization));
  put(&criteria.useDriftFallback,     sizeof(criteria.useDriftFallback));
  put(criteria._pad,                  sizeof(criteria._pad));
  if (releaseCount > 0)
    put(releaseMap.data(), (size_t)releaseCount * sizeof(int32_t));
  for (const auto &entry : luts) {
    put(&entry.period_samples, sizeof(entry.period_samples));
    put(&entry.period_float,   sizeof(entry.period_float));
    const uint32_t n = (uint32_t)entry.points.size();
    putU32(n);
    for (const auto &pt : entry.points) {
      put(&pt.loop_pos, sizeof(pt.loop_pos));
      put(&pt.best_r,   sizeof(pt.best_r));
    }
  }

  // Write tmp file in one shot, then atomically rename.
  const wxString tmpPath = path + wxT(".tmp");
  wxFile f;
  if (!f.Open(tmpPath, wxFile::write)) return false;
  const bool ok = (f.Write(buf.data(), buf.size()) == (wxFileOffset)buf.size());
  f.Close();
  if (!ok) { wxRemoveFile(tmpPath); return false; }

  if (!wxRenameFile(tmpPath, path, true)) {
    wxRemoveFile(tmpPath);
    return false;
  }
  return true;
}

// ── Reader ────────────────────────────────────────────────────────────────────

// Minimal header size (magic + 2×ver + hash + 2×count).
static constexpr size_t GOLUT_FIXED_HEADER_SIZE =
  sizeof(GOLUT_MAGIC) + 4 + 4 + GOLUT_HASH_FIELD + 4 + 4;

// Buffer-based deserializer — zero syscall overhead after initial file read.
struct BufReader {
  const uint8_t *data;
  size_t         pos;
  size_t         size;

  bool Read(void *dst, size_t n) {
    if (pos + n > size) return false;
    memcpy(dst, data + pos, n);
    pos += n;
    return true;
  }
  bool ReadU32(uint32_t &v) { return Read(&v, 4); }
};

bool GOLutCacheReader::Load(
  const wxString &path,
  const wxString &expectedOdfHash,
  uint32_t        expectedReleaseCount,
  bool            headerOnly) {
  m_valid    = false;
  m_lutCount = 0;
  m_releaseMap.clear();
  m_luts.clear();

  if (!wxFileExists(path))
    return false;

  wxFile f;
  if (!f.Open(path, wxFile::read))
    return false;

  // Read entire file into memory — avoids per-field syscall overhead.
  const wxFileOffset fileSize = f.Length();
  if (fileSize < 0 || (size_t)fileSize < GOLUT_FIXED_HEADER_SIZE)
    return false;
  std::vector<uint8_t> buf((size_t)fileSize);
  if (f.Read(buf.data(), buf.size()) != fileSize)
    return false;
  f.Close();

  BufReader r{buf.data(), 0, buf.size()};

  // ── Header ──────────────────────────────────────────────────────────────────
  char magic[8] = {};
  if (!r.Read(magic, sizeof(magic))) return false;
  if (memcmp(magic, GOLUT_MAGIC, sizeof(GOLUT_MAGIC)) != 0) return false;

  uint32_t fmtVer = 0, algVer = 0;
  if (!r.ReadU32(fmtVer)) return false;
  if (!r.ReadU32(algVer)) return false;
  if (fmtVer != GOLUT_FORMAT_VERSION)    return false;
  if (algVer  != GOLUT_ALGORITHM_VERSION) return false;

  char hashField[GOLUT_HASH_FIELD] = {};
  if (!r.Read(hashField, GOLUT_HASH_FIELD)) return false;
  hashField[GOLUT_HASH_FIELD - 1] = '\0';
  if (wxString::FromUTF8(hashField) != expectedOdfHash) return false;

  uint32_t releaseCount = 0, lutCount = 0;
  if (!r.ReadU32(releaseCount)) return false;
  if (!r.ReadU32(lutCount))     return false;
  if (releaseCount != expectedReleaseCount) return false;
  if (lutCount > releaseCount) return false;

  m_lutCount = lutCount;

  // headerOnly: header validated, counts available — skip the bulk data.
  if (headerOnly) {
    m_valid = true;
    return true;
  }

  // Generator criteria (diagnostics only, not validated).
  if (!r.Read(&m_criteria.minScore,             sizeof(m_criteria.minScore)))             return false;
  if (!r.Read(&m_criteria.minCoherence,         sizeof(m_criteria.minCoherence)))         return false;
  if (!r.Read(&m_criteria.maxGapFills,          sizeof(m_criteria.maxGapFills)))          return false;
  if (!r.Read(&m_criteria.requireStabilization, sizeof(m_criteria.requireStabilization))) return false;
  if (!r.Read(&m_criteria.useDriftFallback,     sizeof(m_criteria.useDriftFallback)))     return false;
  if (!r.Read(m_criteria._pad,                  sizeof(m_criteria._pad)))                 return false;

  // ── ReleaseMap ──────────────────────────────────────────────────────────────
  m_releaseMap.resize(releaseCount, -1);
  if (releaseCount > 0)
    if (!r.Read(m_releaseMap.data(), releaseCount * sizeof(int32_t)))
      return false;

  for (int32_t idx : m_releaseMap)
    if (idx != -1 && (uint32_t)idx >= lutCount) return false;

  // ── LUT entries ─────────────────────────────────────────────────────────────
  static constexpr uint32_t GOLUT_MAX_POINTS = 6000;
  m_luts.resize(lutCount);
  for (GOLutEntry &entry : m_luts) {
    if (!r.Read(&entry.period_samples, sizeof(entry.period_samples))) return false;
    if (!r.Read(&entry.period_float,   sizeof(entry.period_float)))   return false;
    uint32_t n = 0;
    if (!r.ReadU32(n)) return false;
    if (n > GOLUT_MAX_POINTS) return false;
    entry.points.resize(n);
    if (n > 0) {
      for (GOLutPoint &pt : entry.points) {
        if (!r.Read(&pt.loop_pos, sizeof(pt.loop_pos))) return false;
        if (!r.Read(&pt.best_r,   sizeof(pt.best_r)))   return false;
      }
    }
  }

  m_valid = true;
  return true;
}

bool GOLutCacheReader::PeekHeader(
  const wxString &path, const wxString &expectedOdfHash) {
  if (!wxFileExists(path))
    return false;

  wxFile f;
  if (!f.Open(path, wxFile::read))
    return false;

  uint8_t buf[GOLUT_FIXED_HEADER_SIZE];
  if (f.Read(buf, sizeof(buf)) != (wxFileOffset)sizeof(buf))
    return false;

  BufReader r{buf, 0, sizeof(buf)};

  char magic[8] = {};
  if (!r.Read(magic, sizeof(magic))) return false;
  if (memcmp(magic, GOLUT_MAGIC, sizeof(GOLUT_MAGIC)) != 0) return false;

  uint32_t fmtVer = 0, algVer = 0;
  if (!r.ReadU32(fmtVer)) return false;
  if (!r.ReadU32(algVer)) return false;
  if (fmtVer != GOLUT_FORMAT_VERSION)    return false;
  if (algVer  != GOLUT_ALGORITHM_VERSION) return false;

  char hashField[GOLUT_HASH_FIELD] = {};
  if (!r.Read(hashField, GOLUT_HASH_FIELD)) return false;
  hashField[GOLUT_HASH_FIELD - 1] = '\0';
  return wxString::FromUTF8(hashField) == expectedOdfHash;
}
