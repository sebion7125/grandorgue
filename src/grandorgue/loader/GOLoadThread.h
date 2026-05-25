/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2023 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOLOADTHREAD_H
#define GOLOADTHREAD_H

#include "threading/GOThread.h"

#include "GOLoadWorker.h"

#include <atomic>
#include <exception>
#include <memory>

class GOLoadThread : private GOLoadWorker, private GOThread {
private:
  /* the main loading loop. It takes objects from the m_CacheObjects
   * concurrently with other threads loads them
   */
  void Entry() override;

  // Captured exception (if any) from the thread
  std::exception_ptr m_exception = nullptr;
  // Flag set when this worker noticed a user abort / cancel token
  std::atomic_bool m_userAbort{false};

public:
  GOLoadThread(
    const GOFileStore &fileStore,
    GOMemoryPool &pool,
    GOCacheObjectDistributor &distributor)
    : GOLoadWorker(fileStore, pool, distributor) {}
  ~GOLoadThread() { Stop(); }

  void Run() { Start(); }

  /**
   * Waits for completions
   * @return true if any exceptions occured. Otherwise - false
   */
  bool CheckExceptions();

  // Exception accessors for main thread to inspect and rethrow if needed
  bool HasException() const { return m_exception != nullptr; }
  std::exception_ptr GetException() const { return m_exception; }

  // Was this thread terminated due to detected user abort?
  bool WasUserAbort() const { return m_userAbort.load(); }
};

#endif
