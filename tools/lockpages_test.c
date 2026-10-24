/* Standalone probe for the "Lock pages in memory" privilege
 * (SeLockMemoryPrivilege): enables the privilege in the process token
 * exactly like GrandOrgue's large-page arenas will, then tries a 2MB
 * MEM_LARGE_PAGES VirtualAlloc. Run by double-click on Windows; the
 * window stays open until Enter is pressed.
 *
 * Build (Linux cross): x86_64-w64-mingw32-gcc -O2 -o lockpages_test.exe
 *   lockpages_test.c
 */
#include <stdio.h>
#include <windows.h>

int main(void) {
  HANDLE token = NULL;
  TOKEN_PRIVILEGES tp;
  const SIZE_T largeMin = GetLargePageMinimum();

  printf("GetLargePageMinimum: %llu bytes\n", (unsigned long long)largeMin);

  if (!OpenProcessToken(
        GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
    printf("FAIL: OpenProcessToken error %lu\n", GetLastError());
    goto done;
  }
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (!LookupPrivilegeValueW(
        NULL, L"SeLockMemoryPrivilege", &tp.Privileges[0].Luid)) {
    printf("FAIL: LookupPrivilegeValue error %lu\n", GetLastError());
    goto done;
  }
  if (!AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL)) {
    printf("FAIL: AdjustTokenPrivileges error %lu\n", GetLastError());
    goto done;
  }
  /* AdjustTokenPrivileges succeeds even when the privilege is NOT in the
   * token - the real verdict is in GetLastError. */
  if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
    printf("FAIL: privilege not assigned to this account (or no fresh "
           "logon since assigning it)\n");
    goto done;
  }
  printf("OK: SeLockMemoryPrivilege enabled in the process token\n");

  {
    const SIZE_T sz = largeMin ? largeMin : (2u << 20);
    void *p = VirtualAlloc(
      NULL, sz, MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES, PAGE_READWRITE);

    if (p) {
      printf("OK: 2MB large-page VirtualAlloc succeeded at %p\n", p);
      printf("\nRESULT: large pages WORK for this account.\n");
      VirtualFree(p, 0, MEM_RELEASE);
    } else {
      DWORD e = GetLastError();
      printf(
        "FAIL: MEM_LARGE_PAGES VirtualAlloc error %lu (%s)\n",
        e,
        e == 1314   ? "ERROR_PRIVILEGE_NOT_HELD"
        : e == 8    ? "ERROR_NOT_ENOUGH_MEMORY - RAM too fragmented"
                    : "unexpected");
    }
  }

done:
  if (token)
    CloseHandle(token);
  printf("\nEnter beendet...\n");
  getchar();
  return 0;
}
