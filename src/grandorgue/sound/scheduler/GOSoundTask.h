/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2024 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDTASK_H
#define GOSOUNDTASK_H

class GOSoundThread;

class GOSoundTask {
public:
  virtual ~GOSoundTask() {}

  virtual unsigned GetGroup() = 0;
  virtual unsigned GetCost() = 0;
  virtual bool GetRepeat() = 0;
  virtual void Run(GOSoundThread *thread = nullptr) = 0;
  virtual void Exec() = 0;

  virtual void Clear() = 0;
  virtual void Reset() = 0;

  enum {
    TREMULANT = 10,
    WINDCHEST = 20,
    AUDIOGROUP = 50,
    AUDIOOUTPUT = 100,
    AUDIORECORDER = 150,
    RELEASE = 160,
    // 180 was GAPFILL, the shadow-prediction task.  Retired: the list
    // dispatches on "handed out", not "finished", so the task fired
    // before the mix was done and NextPeriod()'s Exec() then ran the
    // shadow on the CALLBACK thread.  The shadow now lives on
    // GOSoundConcealment's own FIFO-61 thread, woken by the mix epilog.
    TOUCH = 700,
  };
};

#endif
