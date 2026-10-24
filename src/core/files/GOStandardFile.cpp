/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2023 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOStandardFile.h"

#ifndef __WXMSW__
#include <wx/dir.h>
#include <wx/filename.h>

/**
 * Sample sets are usually authored on Windows, where the filesystem resolves
 * names case-insensitively - so an ODF may say "29_on.BMP" while the disk
 * holds "29_on.bmp" and nobody ever notices. On a case-sensitive filesystem
 * the exact open fails on such a set. This resolves the path component by
 * component, matching each segment case-insensitively against the directory
 * listing when the exact name does not exist. Only called after the exact
 * open has already failed, so the common (correct-case) path costs nothing.
 *
 * Returns true and fills `resolved` if every component could be matched.
 * With several candidates differing only in case the first one found wins -
 * such a set is ambiguous on Windows too.
 */
static bool resolveCaseInsensitive(const wxString &path, wxString &resolved) {
  const wxFileName fn(path);
  wxArrayString segments = fn.GetDirs();

  segments.Add(fn.GetFullName());

  wxString cur = fn.IsAbsolute() ? wxString(wxFILE_SEP_PATH) : wxString(".");

  for (const wxString &seg : segments) {
    if (seg.IsEmpty())
      continue;

    wxString candidate = cur;

    if (!candidate.EndsWith(wxFILE_SEP_PATH))
      candidate += wxFILE_SEP_PATH;
    candidate += seg;
    // "." / ".." and exact-case hits pass through unchanged.
    if (
      seg == wxT(".") || seg == wxT("..") || wxFileExists(candidate)
      || wxDirExists(candidate)) {
      cur = candidate;
      continue;
    }

    wxDir dir(cur);

    if (!dir.IsOpened())
      return false;

    wxString name;
    bool found = false;

    for (bool more = dir.GetFirst(&name); more; more = dir.GetNext(&name))
      if (name.CmpNoCase(seg) == 0) {
        cur = cur.EndsWith(wxFILE_SEP_PATH) ? cur + name
                                            : cur + wxFILE_SEP_PATH + name;
        found = true;
        break;
      }
    if (!found)
      return false;
  }
  resolved = cur;
  return true;
}
#endif

GOStandardFile::GOStandardFile(const wxString &path)
  : m_Path(path), m_Name(path), m_Size(0) {}

GOStandardFile::GOStandardFile(const wxString &path, const wxString &name)
  : m_Path(path), m_Name(name), m_Size(0) {}

GOStandardFile::~GOStandardFile() { Close(); }

const wxString GOStandardFile::GetName() { return m_Name; }

const wxString GOStandardFile::GetPath() { return m_Path; }

size_t GOStandardFile::GetSize() { return m_Size; }

bool GOStandardFile::Open() {
  bool isOk = false;

  // Avoid opening a directory
  if (wxFileExists(m_Path) && m_File.Open(m_Path, wxFile::read)) {
    m_Size = m_File.Length();
    isOk = true;
  }
#ifndef __WXMSW__
  else {
    // Windows-authored set on a case-sensitive filesystem? See
    // resolveCaseInsensitive() above.
    wxString resolved;

    if (
      resolveCaseInsensitive(m_Path, resolved) && wxFileExists(resolved)
      && m_File.Open(resolved, wxFile::read)) {
      m_Path = resolved;
      m_Size = m_File.Length();
      isOk = true;
    }
  }
#endif
  return isOk;
}
void GOStandardFile::Close() { m_File.Close(); }

size_t GOStandardFile::Read(void *buffer, size_t len) {
  ssize_t read = m_File.Read(buffer, len);
  if (read == wxInvalidOffset)
    return 0;
  return read;
}
