# module-rt: failure to set nice-level forces ALL realtime scheduling through RTKit, even when direct scheduling is available and verified working

## Summary

When `RLIMIT_RTPRIO` is properly set up and module-rt's own privilege check **succeeds** ("can set rt prio to 88"), but setting the nice-level fails (no `RLIMIT_NICE`/`CAP_SYS_NICE`), module-rt switches **the entire module** to the RTKit/portal path — including realtime priorities for the data-loop threads. The result: the server's data-loop threads end up clamped to RTKit's much lower ceiling (default 20) instead of the requested/verified `rt.prio` (88), silently inverting the intended priority hierarchy.

This contradicts the documented behavior ("If this is not set up … then this module will fall back to using the Portal Realtime DBus API or RTKit" — https://docs.pipewire.org/page_module_rt.html): the rlimits *were* set up, and the module itself verified that direct scheduling works.

## Environment

- PipeWire 1.6.2 (Ubuntu 26.04, kernel 7.0.0-28-generic)
- pipewire/wireplumber/pipewire-pulse running as systemd user services
- `RLIMIT_RTPRIO` available to the user manager (pam_limits, `@audio` rtprio 95) — verified by module-rt's own check succeeding
- No `RLIMIT_NICE` for the user (Ubuntu default)
- rtkit-daemon running (initially default `--max-realtime-priority=20`)

## Steps to reproduce

1. Configure `RLIMIT_RTPRIO` so that direct `pthread_setschedparam` works for the pipewire user service (e.g. pam_limits + user-manager limit), but leave `RLIMIT_NICE` at its default (nice -11 not permitted).
2. Restart pipewire.
3. Observe thread priorities: `ps -eLo rtprio,cls,comm | grep data-loop`

## Expected

Data-loop threads at `rt.prio` (88, SCHED_FIFO) via direct scheduling — the path module-rt itself just verified. A failure to set the *nice level* should at most degrade the nice level (or fetch only the nice level via RTKit), not the realtime priorities.

## Actual

Data-loop threads at **RR 20** (RTKit ceiling). Debug log (`PIPEWIRE_DEBUG="mod.rt:D"`):

```
D mod.rt [module-rt.c:594:check_realtime_privileges]: can set rt prio to 88
W mod.rt [module-rt.c:624:set_nice]: could not set nice-level to -11: Permission denied
D mod.rt [module-rt.c:941:rtkit_get_bus]: enter rtkit get bus
D mod.rt [module-rt.c:1172:pipewire__module_init]: initialized using RTKit
I mod.rt [module-rt.c:627:set_nice]: main thread nice level set to -11
I mod.rt [module-rt.c:829:do_make_realtime]: clamping requested priority 88 for thread 24622 between 1 and 20
I mod.rt [module-rt.c:839:do_make_realtime]: acquired realtime priority 20 for thread 24622 using RTKit
```

Note the sequence: the direct check succeeds, then the nice-level failure flips `use_rtkit = !!dbus` in `pipewire__module_init`, and from then on `do_make_realtime` goes through RTKit and clamps 88 → 20.

## Impact

This is not cosmetic. JACK/native clients in a login session (with pam_limits) get their client data-loops at FF 83, while the *server's* driver data-loop — the thread that actually times the graph cycle — sits at RR 20, i.e. **below the clients it drives**. Under high DSP load the clients starve the driver loop: cycles are delivered late and the sound device underruns, while the clients' own timing counters look healthy. We chased audible dropouts on a 20-core machine for days before finding this. Raising rtkit's `--max-realtime-priority` is only a partial workaround (and is further hampered by xdg-desktop-portal caching RTKit's limits from login, so a runtime rtkit reconfiguration is not picked up until the portal is restarted — arguably a separate issue).

## Suggested fix

In `pipewire__module_init`, when `check_realtime_privileges()` succeeds but `set_nice()` fails:

- keep direct scheduling for realtime priorities (`use_rtkit = false` for the RT path),
- optionally use RTKit/portal *only* for the nice level (the module already does exactly this successfully — see the "main thread nice level set to -11" line above, which went through RTKit),
- or at least log a prominent warning that the effective realtime ceiling just dropped from the verified `rt.prio` to RTKit's maximum.

## Workaround for affected users

Grant the nice limit so the direct path is kept, e.g. `/etc/security/limits.d/`: `@audio - nice -19` (plus rtprio), then restart the session — module-rt then stays on direct scheduling with FF `rt.prio`.
