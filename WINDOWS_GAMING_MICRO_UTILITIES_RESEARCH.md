# Research: single-purpose Windows gaming-performance utilities

**Research date:** 2026-09-03  
**Target:** tiny, measurable Windows utilities—not tweak packs, debloaters, process
killers, or generic “game boosters.”

## Executive conclusion

The best opportunity is not another tool that permanently sets priority or clears
caches. It is a **closed-loop utility** that observes one source of bad frame-time
outliers and applies one reversible policy only while that source is active.

The three strongest prototypes are:

1. **IRQ Shelter**—measure which logical processors service disruptive DPC/ISR
   work, then keep the game off only those processors.
2. **Hybrid Thread Anchor**—identify the persistently frame-critical game threads
   and express a soft preference for performant CPU sets, without hard-pinning the
   entire game.
3. **VRAM Headroom Bell**—warn, log, and optionally quiesce an allow-listed GPU
   background consumer when DXGI reports local-memory budget pressure.

These are attractive because each has an objective trigger, a reversible action,
negligible steady-state cost, and an A/B test based on PresentMon plus ETW. Expected
gains are mostly in tail latency, not headline average FPS. No utility can honestly
promise a universal 2–5% gain: each is intended to activate only on machines where
telemetry proves its particular pathology exists.

## Research method and evidence standard

This is a feasibility study, not a list of folklore tweaks. Ideas were screened
against four questions:

* Can Windows expose the problem through a documented API or ETW provider?
* Can an out-of-process utility apply a narrow, reversible mitigation?
* Can the effect be measured in frame-time percentiles and correlated with the
  trigger?
* Is the proposal materially narrower than Process Lasso, timer-resolution tools,
  driver control panels, or “boosters”?

Primary references include Microsoft API/driver documentation, Microsoft DirectX
engineering posts, Intel's open-source PresentMon, GPUOpen material, and relevant
open-source implementations. “Proves” below means the cited source proves a
mechanism or the availability of telemetry—not that it proves a particular FPS
uplift in every game. Performance estimates are hypotheses to validate.

### Benchmark contract for every prototype

Use the same replay/save, fixed camera path, resolution, driver, power plan, and
background workload. Randomize at least 10 paired A/B runs after warm-up. Capture:

* PresentMon `MsBetweenPresents`, `MsUntilDisplayed`, dropped/late presents, and
  presentation mode;
* median, 99th-percentile and 99.9th-percentile frame time (report milliseconds,
  not only “1% FPS”); worst one-second window; and time above 1.5×/2× budget;
* ETW context switches, ready time, sampled CPU, hard faults, disk I/O, ISR/DPC,
  and GPU queues;
* input-to-photon only when an LDAT/high-speed-camera setup exists—never infer it
  merely from FPS.

[PresentMon](https://github.com/GameTechDev/PresentMon) provides vendor-neutral ETW
capture of presentation data. [GPUView](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/using-gpuview)
and [Windows Performance Analyzer](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/windows-performance-analyzer)
cover GPU/CPU scheduling and ETW correlation. PresentMon itself consumes measurable
resources, so it should be an evaluation tool or low-duty sampler rather than a
mandatory high-rate component in every shipping daemon.

## Opportunity map

### 1. IRQ Shelter

1. **Single problem:** network, USB, audio, or storage ISR/DPC activity shares a
   logical processor with the game's frame-critical thread and causes sporadic
   preemption.
2. **Why it matters:** DPCs run above ordinary game-thread priority. A modest total
   CPU load can therefore produce long tail frames when bursts land on the wrong
   CPU; average FPS may barely move.
3. **Exact behavior:** perform a 15–30 second ETW calibration, attribute ISR/DPC
   duration and burstiness per logical processor and driver, then assign the game a
   *soft* default CPU-set mask excluding at most one demonstrated “noisy” logical
   processor per processor group. Re-evaluate slowly and restore the original sets
   on exit. The first version should advise-only; an advanced, explicitly opted-in
   mode can suggest persistent device interrupt-affinity changes that take effect
   after reboot.
4. **Expected benefit:** no average-FPS promise; plausibly fewer 99th/99.9th-percentile
   spikes and improved input consistency on DPC-troubled systems. Zero expected
   benefit when DPC time is already diffuse or trivial.
5. **Runtime overhead:** calibration around 0.2–1 CPU%; steady-state below 0.05% if
   ETW is sampled in short windows; roughly 10–30 MB during capture.
6. **Difficulty:** **Moderate**.
7. **Prototype complexity:** about 1–2 weeks: ETW consumer, per-CPU scorer, CPU-set
   transaction/rollback, topology awareness, and benchmark logger.
8. **APIs/techniques:** NT Kernel Logger/ETW Interrupt and DPC events;
   `GetSystemCpuSetInformation`; `SetProcessDefaultCpuSets` or
   `SetThreadSelectedCpuSets`; processor-group APIs; device-instance lookup. A
   persistent interrupt-affinity mode would use the documented interrupt affinity
   policy registry schema and require elevation/reboot.
9. **Evidence:** Windows documents [CPU Sets](https://learn.microsoft.com/en-us/windows/win32/procthread/cpu-sets),
   [ETW interrupt events](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/interrupt),
   and the driver's [interrupt affinity policy](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/interrupt-affinity-and-priority).
   Microsoft's Interrupt-Affinity Policy Tool and WPT demonstrate both control and
   attribution; LatencyMon demonstrates consumer demand for driver/DPC diagnosis.
10. **Different/better:** existing tools either report aggregate DPC latency or set
    a static affinity. This closes the loop around the launched game's topology,
    excludes the minimum capacity, and can prove correlation before acting.
11. **Risks:** removing a CPU can hurt a CPU-saturated game; SMT siblings share
    resources; moving an interrupt may worsen device latency; kernel ETW often
    requires elevation; anti-cheat may dislike handle access. Never change persistent
    device policy automatically.

### 2. Hybrid Thread Anchor

1. **Single problem:** a game's one or two latency-critical threads migrate between
   heterogeneous P/E cores (or distant cache domains), creating variable execution
   time.
2. **Why it matters:** Windows 11 and Intel Thread Director collaborate well in the
   general case, but thread phase changes, power policy, overlays, and imperfect game
   hints can still cause migrations. Pinning the whole process is worse: worker
   parallelism is lost and Windows cannot load-balance.
3. **Exact behavior:** during a short learning window, use ETW context-switch and
   sampled-profile data to rank long-lived threads by CPU time immediately preceding
   presents. Apply `SetThreadSelectedCpuSets` to only the top one or two stable
   candidates, preferring performant sets inside one last-level-cache domain. Remove
   the hint when confidence falls, thread IDs recycle, or the game changes phase.
4. **Expected benefit:** potentially 1–5% better 1% lows and fewer outlier frames in
   affected CPU-bound hybrid systems; usually no average-FPS change.
5. **Runtime overhead:** under 0.2% after a short learning capture; a few MB state.
6. **Difficulty:** **Moderate–Hard** because render-thread inference is the product.
7. **Prototype complexity:** 2–4 weeks for x64 Windows 11, one processor group, and
   an allow-list; another 2–4 weeks for robust topology/phase handling.
8. **APIs/techniques:** ETW CSwitch/ReadyThread/SampledProfile plus Present events;
   Toolhelp thread enumeration; `GetSystemCpuSetInformation` flags and efficiency
   class; `SetThreadSelectedCpuSets`; `GetLogicalProcessorInformationEx`; reversible
   handle bookkeeping.
9. **Evidence:** Microsoft's [heterogeneous scheduling guidance](https://learn.microsoft.com/en-us/windows/win32/procthread/scheduling-on-heterogeneous-processors)
   explains efficiency classes and scheduling policy. The [CPU Sets API](https://learn.microsoft.com/en-us/windows/win32/procthread/cpu-sets)
   exists specifically as a scheduler-compatible placement mechanism. Bitsum's
   [Process Lasso](https://bitsum.com/processlasso/) shows affinity automation has a
   market, but it generally exposes process-level policy rather than learned
   frame-thread placement.
10. **Different/better:** soft, per-thread, evidence-driven, cache-domain-aware, and
    transient—not a permanent whole-process affinity mask.
11. **Risks:** misidentifying a worker/render thread can serialize work; a game's own
    thread policy can fight the utility; P-cores may be thermally constrained;
    opening protected threads can fail. Disable for anti-cheat-protected games unless
    only documented access succeeds.

### 3. VRAM Headroom Bell

1. **Single problem:** a browser, animated wallpaper, capture app, or overlay pushes
   local video-memory usage near the OS-provided budget just as a game streams
   assets, causing eviction/residency stalls.
2. **Why it matters:** WDDM budgets change dynamically and are not the physical VRAM
   size. Crossing budget can cause paging and severe isolated frames even when
   average GPU utilization looks normal.
3. **Exact behavior:** call `IDXGIAdapter3::QueryVideoMemoryInfo`, register for budget
   change notification, and record budget/usage beside PresentMon markers. When
   sustained headroom drops below a learned threshold, show a specific attribution
   (“browser GPU process: 1.4 GB”) and optionally suspend or ask an *explicitly
   allow-listed* background GPU app to pause. Resume it immediately when the game
   exits. Version one is diagnostic and has no destructive action.
4. **Expected benefit:** potentially large removal of rare hitches on VRAM-limited
   settings; no benefit and almost no cost when headroom is healthy. Average FPS is
   not the target.
5. **Runtime overhead:** event-driven; below 0.02% CPU and under 10 MB RAM, excluding
   optional attribution sampling.
6. **Difficulty:** **Easy–Moderate** for budget telemetry; **Hard** for reliable
   per-process VRAM attribution across vendors.
7. **Prototype complexity:** 3–5 days for recorder/alert; 2–3 weeks for attribution
   and safe allow-listed actions.
8. **APIs/techniques:** DXGI 1.4 `IDXGIAdapter3`,
   `QueryVideoMemoryInfo`, `RegisterVideoMemoryBudgetChangeNotificationEvent`, D3DKMT
   statistics or GPU ETW for attribution, process lifecycle notifications.
9. **Evidence:** Microsoft documents [video-memory budgets](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nn-dxgi1_4-idxgiadapter3)
   and [residency](https://learn.microsoft.com/en-us/windows/win32/direct3d12/residency).
   GPUView exposes paging queues, while tools such as PresentMon/OCAT demonstrate
   correlating GPU telemetry with bad presents.
10. **Different/better:** Task Manager reports GPU memory but does not create a
    game-session headroom timeline, correlate budget crossings with hitches, or
    perform one reversible, allow-listed quiescence action.
11. **Risks:** reported usage includes shared/aliased allocations; suspending a
    browser may break calls/recording; a threshold can flap; eviction can be caused
    by the game itself. Use hysteresis and default to alert-only.

### 4. I/O Collision Gate

1. **Single problem:** unrelated low-value disk writes/reads collide with a game's
   bursty asset reads on a saturated SSD or decompression path.
2. **Why it matters:** NVMe has high throughput but finite queues, thermal limits,
   and tail latency. Indexing, sync, updates, or recording can lengthen asset-ready
   time and starve the render pipeline.
3. **Exact behavior:** learn game-read bursts from ETW File I/O/Disk I/O. Only while
   an active burst and elevated storage latency coincide, lower I/O priority and
   CPU QoS of user-approved background writers; restore within hundreds of
   milliseconds. Do not kill processes, purge caches, or throttle system-critical
   services.
4. **Expected benefit:** fewer traversal/texture-streaming hitches and better 0.1%
   lows under real contention; zero gain on uncongested storage.
5. **Runtime overhead:** 0.1–0.5% CPU during trace windows, below 0.05% otherwise.
6. **Difficulty:** **Moderate–Hard**.
7. **Prototype complexity:** 2–4 weeks with an allow-list and advisory first mode.
8. **APIs/techniques:** kernel File I/O and Disk I/O ETW, process I/O counters,
   `SetPriorityClass(PROCESS_MODE_BACKGROUND_BEGIN)` where applicable, EcoQoS via
   `SetProcessInformation`, job-object CPU controls, transactional restoration.
9. **Evidence:** Windows documents [background processing mode](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setpriorityclass)
   as lowering resource scheduling priorities, and WPT exposes disk service time and
   process attribution. Microsoft's [DirectStorage overview](https://devblogs.microsoft.com/directx/directstorage-is-coming-to-pc/)
   explains why modern games increasingly rely on latency-sensitive parallel asset
   I/O and decompression.
10. **Different/better:** acts for sub-second, telemetry-proven collisions, not an
    entire gaming session and not against arbitrary processes.
11. **Risks:** background mode changes CPU and memory priorities as well as I/O;
    throttling capture/voice data may cause loss; ETW volume can cost more than the
    problem. Require opt-in targets and minimum latency/queue thresholds.

### 5. Core-Wake Smoother

1. **Single problem:** a game with periodic parallel bursts repeatedly waits for
   parked/deep-idle logical processors to become fully productive.
2. **Why it matters:** boost and idle transitions are fast but not free, and their
   latency can appear at the start of a frame job burst. Globally disabling C-states
   wastes substantial power and thermal headroom.
3. **Exact behavior:** detect a repeatable burst cadence from per-core ETW activity;
   place one extremely low-duty waitable-timer pulse on the specific soon-needed CPU
   set just before the learned burst. Stop immediately if no frame-time improvement
   appears. This is a bounded experiment, not a permanent busy loop.
4. **Expected benefit:** sub-millisecond reduction in some tail frames on aggressive
   mobile/balanced policies; likely no desktop benefit. Could improve 1% lows by a
   few percent only in a positively diagnosed case.
5. **Runtime overhead:** target below 0.1% of one core; power cost is the key metric.
6. **Difficulty:** **Hard**.
7. **Prototype complexity:** 3–6 weeks including safe phase prediction and energy
   measurement.
8. **APIs/techniques:** ETW CSwitch and power/idle providers, CPU Sets, high-resolution
   waitable timers, `CallNtPowerInformation`/energy telemetry where available.
9. **Evidence:** Windows exposes [processor power-policy settings](https://learn.microsoft.com/en-us/windows-hardware/customize/power-settings/configure-processor-power-management-options)
   and ETW power events. Academic and systems work on race-to-idle establishes that
   wake-up and performance-state selection trade latency for energy, but it does not
   prove this speculative pulse helps any particular game.
10. **Different/better:** scoped to a proven frame phase and a few CPUs, with an
    automatic no-benefit shutoff; unlike disabling parking/C-states globally.
11. **Risks:** extra heat can reduce boost and make performance worse; timer jitter
    may miss the phase; laptops lose battery; security software may flag affinity
    pulses. Ship only after instrumented experiments.

### 6. Foreground QoS Corrector

1. **Single problem:** a game or launcher child inherits or acquires Windows power
   throttling/EcoQoS despite being latency-critical in the foreground.
2. **Why it matters:** EcoQoS intentionally favors efficiency and may steer work to
   efficient processors or lower frequencies. Incorrect classification can hurt a
   CPU-bound frame thread.
3. **Exact behavior:** inspect power-throttling state for the foreground game's
   process tree, log it, and clear only `PROCESS_POWER_THROTTLING_EXECUTION_SPEED`
   while the actual game window is foreground. Restore the original state on focus
   loss/exit. Do nothing when the bit was not enabled.
4. **Expected benefit:** potentially material FPS/latency recovery in the rare
   misclassified case; exactly zero otherwise.
5. **Runtime overhead:** event-driven, effectively zero.
6. **Difficulty:** **Easy**.
7. **Prototype complexity:** 2–4 days including process-tree and rollback tests.
8. **APIs/techniques:** WinEvent foreground hook, process creation ETW or WMI,
   `GetProcessInformation`/`SetProcessInformation` with
   `PROCESS_POWER_THROTTLING_STATE`.
9. **Evidence:** Microsoft documents [process power throttling](https://learn.microsoft.com/en-us/windows/win32/procthread/process-power-throttling)
   and [EcoQoS](https://devblogs.microsoft.com/performance-diagnostics/introducing-ecoqos/),
   including the execution-speed control bit and efficiency-oriented behavior.
10. **Different/better:** not “High priority always”; it is a state auditor that
    changes precisely one QoS bit only when demonstrably wrong.
11. **Risks:** launchers/background helpers may be intentionally throttled; clearing
    the hint increases power and heat; access may fail. Default to the primary game
    executable, not descendants.

### 7. Timer Occlusion Guard

1. **Single problem:** a borderless/minimized/occluded game's timing behavior changes
   because Windows no longer honors high-resolution periodic timer requests for an
   occluded process.
2. **Why it matters:** since Windows 10 version 2004 timer resolution is more
   process-scoped, and Windows 11 may withhold high resolution from fully occluded
   window-owning processes. Some games or latency tests can therefore develop
   cadence changes during overlays, display switching, or streaming setups.
3. **Exact behavior:** detect the affected OS/version and game visibility state,
   measure effective wake intervals, and identify an unexpected occlusion transition.
   A cooperative per-game shim can call `SetProcessInformation` with
   `PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION` cleared and make its own matched
   `timeBeginPeriod` request; the safe universal product should remain diagnostic
   unless the game vendor opts into the shim.
4. **Expected benefit:** restores intended timer cadence in this narrow failure mode;
   possible input/frame-pacing improvement, not average GPU FPS.
5. **Runtime overhead:** below 0.01% CPU; timer-resolution requests may increase
   system power consumption.
6. **Difficulty:** **Easy** diagnostic, **Hard/risky** injection.
7. **Prototype complexity:** 3–5 days diagnostic; 2–3 weeks for a signed cooperative
   shim and compatibility work.
8. **APIs/techniques:** `NtQueryTimerResolution`, `QueryUnbiasedInterruptTime`,
   window occlusion/foreground events, `timeBeginPeriod`, process power-throttling
   information.
9. **Evidence:** Microsoft's [`timeBeginPeriod` documentation](https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod)
   explicitly describes the Windows 10 2004 rule and Windows 11 occlusion behavior.
   Existing TimerTool/ISLC-style utilities prove demand but often present a global
   number without process/visibility diagnosis.
10. **Different/better:** explains a concrete per-process state transition and avoids
    pretending that forcing 0.5 ms system-wide universally reduces latency.
11. **Risks:** DLL injection conflicts with anti-cheat and should not be used there;
    smaller periods consume energy; many modern games use high-resolution waitable
    timers/spin waits and will not benefit.

### 8. Shader Cache Health Sentinel

1. **Single problem:** external cleaners, synchronization tools, permissions, or
   unexpectedly frequent driver/game cache invalidation cause shader/pipeline caches
   to be repeatedly rebuilt during play.
2. **Why it matters:** synchronous shader or pipeline-state compilation is a known
   cause of one-off, very long frames. A generic cleaner may silently erase the very
   data intended to prevent recurrence.
3. **Exact behavior:** observe known DirectX/vendor/game shader-cache directories and
   ETW file activity, fingerprint cache generations, and mark deletion/rebuild events
   on a frame-time trace. Alert on repeated churn or low free space. Optionally place
   only documented cache paths on a backup/exclusion recommendation—never copy caches
   between driver versions or download opaque binaries.
4. **Expected benefit:** prevents recurring compilation stutter rather than improving
   warm-run FPS; very high perceived benefit when churn is the cause.
5. **Runtime overhead:** event-driven directory watches; under 0.05% CPU and a few MB.
6. **Difficulty:** **Easy–Moderate**.
7. **Prototype complexity:** 1 week for DirectX cache and correlation; ongoing small
   adapters for vendors/games.
8. **APIs/techniques:** `ReadDirectoryChangesW`, USN journal (optional), file ETW,
   hashes/metadata, PresentMon marker correlation, disk-free-space APIs.
9. **Evidence:** Microsoft explains Direct3D 12 [pipeline-state caching](https://learn.microsoft.com/en-us/windows/win32/direct3d12/managing-graphics-pipeline-state-in-direct3d-12)
   and its cached PSO mechanism; the DirectX team's [Shader Compilation article](https://devblogs.microsoft.com/directx/shader-compilation-in-the-age-of-pbr/)
   describes compilation scale. Valve's Steam shader pre-caching and Mesa's pipeline
   cache history further support the mechanism, though they are not proof that
   Windows cache deletion is common.
10. **Different/better:** it is a cache *forensics and preservation* tool, not another
    cache deleter and not a fake universal “precompiler” lacking game PSO inputs.
11. **Risks:** paths/formats change; watching huge trees can be costly; stale caches
    are sometimes correctly invalidated; antivirus exclusions reduce security and
    should not be automated.

### 9. Memory-Pressure Priority Shield

1. **Single problem:** under real commit/physical-memory pressure, low-value
   background working sets compete with the game, causing game-page trimming and
   hard/soft fault bursts.
2. **Why it matters:** a hard fault or compressed-page retrieval on a frame-critical
   path can create a large hitch. “Clear standby memory” is counterproductive because
   it discards useful cache; the correct response is selective and pressure-gated.
3. **Exact behavior:** monitor commit, available memory, per-process working-set/fault
   rates, and Memory ETW. When—and only when—game faults correlate with low memory,
   apply low memory priority/EcoQoS to an allow-list of background cache-heavy
   processes. Restore after pressure subsides. Never empty standby lists and never
   force a huge game working-set minimum.
4. **Expected benefit:** better 0.1% lows on RAM-constrained systems; no average-FPS
   gain and no action on healthy machines.
5. **Runtime overhead:** 1 Hz counters below 0.02% CPU; ETW diagnostic burst only.
6. **Difficulty:** **Moderate**.
7. **Prototype complexity:** 1–2 weeks for diagnosis and reversible allow-list QoS.
8. **APIs/techniques:** `GetPerformanceInfo`, `GlobalMemoryStatusEx`,
   `QueryWorkingSetEx`, process memory counters, Memory ETW, process memory-priority
   information (`SetProcessInformation` where supported), EcoQoS.
9. **Evidence:** Microsoft documents [working-set behavior](https://learn.microsoft.com/en-us/windows/win32/memory/working-set)
   and [memory priority](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setprocessinformation).
   RAMMap/WPA demonstrate standby, fault, and per-process attribution. This evidence
   argues against blind standby-list clearing as much as it supports selective policy.
10. **Different/better:** activates on correlated game faults and preserves the file
    cache; it does not market lower “used RAM” as an optimization.
11. **Risks:** demoted apps may fault later; changing another process can fail or
    disrupt audio/streaming; compression behavior varies. Advisory mode and a strict
    allow-list are essential.

### 10. Overlay Present-Path Canary

1. **Single problem:** an overlay, capture hook, HDR/window transition, or compositor
   condition moves a game from a low-latency independent-flip path to composed copy,
   producing worse latency/pacing.
2. **Why it matters:** identical FPS counters can conceal different presentation
   modes and display latency. The cause may appear only after a notification or
   overlay opens.
3. **Exact behavior:** consume Present ETW at low duty cycle, establish the normal
   presentation mode, and notify/log the exact process/window event that coincides
   with a sustained downgrade. Offer one-click disabling only through an overlay's
   supported interface or user-configured process—not injection or blanket killing.
4. **Expected benefit:** diagnostic path to restore frame pacing/input latency; no
   benefit while the optimal present path remains active.
5. **Runtime overhead:** 0.1–0.5% while tracing; duty-cycle after learning.
6. **Difficulty:** **Moderate**.
7. **Prototype complexity:** 1–2 weeks using PresentMon libraries and WinEvent hooks.
8. **APIs/techniques:** PresentMon/PresentData ETW consumer, DWM events, window and
   process lifecycle hooks, optional supported overlay CLI/API integrations.
9. **Evidence:** PresentMon classifies [hardware independent flip, composed flip, and
   copy modes](https://github.com/GameTechDev/PresentMon/blob/main/README.md).
   Microsoft's [Optimizations for windowed games](https://support.microsoft.com/en-us/windows/optimizations-for-windowed-games-in-windows-11-3f006843-2c7e-4ed0-9a5e-f9389e535952)
   confirms presentation-path choices affect latency for compatible DX10/11 games.
10. **Different/better:** a regression canary with event correlation, rather than an
    overlay remover or another FPS overlay.
11. **Risks:** ETW interpretation is complex; alt-tab naturally changes modes;
    capture tools may be intentional; continuous PresentMon overhead can distort
    low-end results.

### 11. Thermal Boost Variance Sentinel

1. **Single problem:** short CPU/GPU temperature, power, or current-limit episodes
   create inconsistent clocks and tail frames even though average temperature looks
   acceptable.
2. **Why it matters:** boost algorithms react on short timescales. A fan curve or
   background GPU consumer can create frequency oscillation that resembles engine
   stutter.
3. **Exact behavior:** correlate frame outliers with vendor-exposed throttle reason,
   effective clock, temperature, and power. If correlation is strong, apply exactly
   one safe reversible action: request a user-defined OEM cooling profile or pause an
   allow-listed animated background app. Primarily produce evidence, not overclock.
4. **Expected benefit:** improved sustained 1% lows on thermally marginal laptops or
   small-form-factor PCs; none on unconstrained hardware.
5. **Runtime overhead:** 2–10 Hz polling, usually below 0.1% CPU.
6. **Difficulty:** **Moderate–Hard** due to fragmented vendor telemetry/control.
7. **Prototype complexity:** 1 week for a single GPU/vendor; 4+ weeks cross-vendor.
8. **APIs/techniques:** NVAPI/NVML, AMD ADLX, Intel PresentMon GPU telemetry, ACPI/OEM
   APIs where officially supported; PresentMon correlation.
9. **Evidence:** Intel's [PresentMon service](https://github.com/GameTechDev/PresentMon)
   exposes GPU telemetry on supported hardware; AMD [ADLX](https://gpuopen.com/adlx/)
   and NVIDIA [NVML](https://developer.nvidia.com/management-library-nvml) expose
   management/telemetry interfaces. These prove observability, not universal control.
10. **Different/better:** frame-linked throttle-cause diagnosis with one safe OEM
    profile action, not arbitrary voltage/clock tuning.
11. **Risks:** sensor polling can contend with other monitors; vendor APIs differ;
    aggressive cooling adds noise; forcing clocks can reduce efficiency or stability
    and is deliberately outside scope.

### 12. Thread Priority-Inversion Scout

1. **Single problem:** a frame-critical game thread repeatedly blocks behind a
   lower-priority in-process worker or external helper holding a synchronization
   resource.
2. **Why it matters:** raising the whole game to High priority does not fix lock
   ownership and can starve the very helper needed to release work. The symptom is a
   ready/wait chain aligned with long frames.
3. **Exact behavior:** capture short ETW windows after outliers, reconstruct wait and
   ready chains, and report stable culprit thread/module patterns. A narrow opt-in
   mitigation may temporarily raise an external helper from Below Normal to Normal;
   it should not manipulate unknown in-process thread priority without developer
   cooperation.
4. **Expected benefit:** potentially eliminates repeatable spikes in affected games;
   diagnostic-only for most titles.
5. **Runtime overhead:** near zero until an outlier arms a short trace; trace windows
   around 0.5–2% CPU depending on stack walking.
6. **Difficulty:** **Hard**.
7. **Prototype complexity:** 4–8 weeks for credible wait-chain inference and symbols.
8. **APIs/techniques:** ETW CSwitch/ReadyThread with stack walking, Wait Chain
   Traversal API, DIA/symbol resolution, thread/process priority queries.
9. **Evidence:** Microsoft documents [Wait Chain Traversal](https://learn.microsoft.com/en-us/windows/win32/debug/wait-chain-traversal)
   and WPA's [CPU scheduling analysis](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/cpu-analysis). These mechanisms support diagnosis; safe automatic repair remains
   unproven.
10. **Different/better:** frame-triggered priority-inversion forensics rather than a
    blanket priority knob.
11. **Risks:** many waits are intentional GPU/fence waits; missing private symbols
    limits attribution; priority changes can cause starvation; protected processes
    reject access. Keep remediation conservative.

## Ideas deliberately rejected or demoted

* **Automatic standby-list purging:** throws away useful cached game data and treats
  lower “used memory” as success.
* **Universal 0.5 ms timer service:** post-2004 timer behavior is process-dependent;
  lower timer periods increase power and do not automatically lower end-to-end input
  latency.
* **Whole-game High/Realtime priority:** can starve audio, input, networking, storage,
  and system threads; it does not resolve locks.
* **Static all-P-core affinity:** discards scheduler flexibility and can overload a
  few cores. Per-thread soft CPU sets are the more defensible experiment.
* **HPET/platform-clock togglers:** boot flags change timing/platform behavior and
  have inconsistent, hardware-dependent outcomes; not a safe tiny runtime utility.
* **Shader “precompiler” without game integration:** a third party generally lacks
  shader variants, pipeline state, content, and legally redistributable cache data.
* **VRAM cleaner:** an external tool cannot safely decide which game allocations to
  evict; forced eviction can create the hitch it claims to solve.
* **GPU keep-awake dummy workload:** consumes thermal/power headroom and can reduce
  boost. Diagnose power transitions instead.
* **Dynamic interrupt re-affinity every second:** device policies are not a real-time
  control loop and may require restart/reboot. Runtime game CPU-set shielding is safer.
* **Automatic service suspension:** too close to a process killer and can break
  networking, audio, capture, updates, or security. All proposed cross-process
  actions are allow-listed and tied to a measured collision.

## Ranked top 10

Scores are comparative, 1–5. **Composite** is the product of potential impact,
implementation ease, uniqueness, and low-overhead fit; it deliberately rewards
small shippable experiments. Impact means *conditional* tail-latency potential, not
universal average FPS.

| Rank | Concept | Impact | Ease | Unique | Low overhead | Composite | Why it ranks here |
|---:|---|---:|---:|---:|---:|---:|---|
| 1 | IRQ Shelter | 4 | 4 | 5 | 5 | **400** | Strong tail-latency mechanism; adaptive per-CPU action is unusually focused. |
| 2 | VRAM Headroom Bell | 4 | 5 | 4 | 5 | **400** | DXGI makes a safe telemetry MVP extremely quick; severe hitches are measurable. |
| 3 | Hybrid Thread Anchor | 4 | 3 | 5 | 5 | **300** | High relevance on hybrid CPUs; render-thread inference is the hard differentiator. |
| 4 | Foreground QoS Corrector | 3 | 5 | 3 | 5 | **225** | Almost free to build/run, though the bad state is likely uncommon. |
| 5 | Shader Cache Health Sentinel | 4 | 4 | 4 | 3 | **192** | Prevents recurring catastrophic hitches; game/vendor path maintenance lowers score. |
| 6 | Overlay Present-Path Canary | 3 | 4 | 4 | 4 | **192** | Excellent objective signal and useful diagnosis; mitigation often remains manual. |
| 7 | Memory-Pressure Priority Shield | 3 | 4 | 3 | 5 | **180** | Safer alternative to RAM cleaners but only helps constrained machines. |
| 8 | I/O Collision Gate | 4 | 3 | 4 | 3 | **144** | Valuable during streaming contention, with nontrivial attribution and safety work. |
| 9 | Thermal Boost Variance Sentinel | 3 | 3 | 3 | 4 | **108** | Useful on laptops; vendor fragmentation limits quick universal delivery. |
| 10 | Timer Occlusion Guard | 2 | 4 | 3 | 4 | **96** | Real documented edge case, but safe out-of-process remediation is limited. |

Core-Wake Smoother (high risk of negative thermal effect) and Thread
Priority-Inversion Scout (excellent diagnostic, weak automatic remediation and high
complexity) fall outside the top ten.

## The smartest first three prototypes

### First: VRAM Headroom Bell (telemetry-only MVP)

It has the shortest path to a trustworthy result: the OS supplies an event-driven
budget API, the daemon can be practically invisible, and no risky optimization is
needed to ship value. Build a trayless logger that writes budget, usage, headroom,
process/game, and frame-outlier timestamps to CSV. Test deliberately with a VRAM
pressure generator, then add attribution and allow-listed actions only if budget
crossings predict outliers.

**Go/no-go:** in controlled pressure tests, budget/headroom events should predict a
statistically higher rate of >2×-budget frames. If not, stop before building control.

### Second: IRQ Shelter (advisory, then soft CPU sets)

This is the best mix of novelty and competitive-gaming relevance. Start by producing
a per-CPU DPC heat map and an offline recommendation. Next A/B a soft game CPU-set
exclusion; do not touch device affinity in the MVP. It has a crisp success metric:
outlier frames coincident with DPC bursts should fall without reducing average CPU
throughput.

**Go/no-go:** require both correlation before action and improved paired-run 99th or
99.9th percentile without a statistically meaningful average-FPS regression.

### Third: Hybrid Thread Anchor (allow-listed experimental build)

It is harder, but it develops the most reusable technical asset: correlating game
thread execution with presents. Restrict the MVP to one hybrid-CPU family and a few
repeatable CPU-bound games. Compare (a) Windows default, (b) whole-process P-core
affinity, and (c) learned per-thread selected CPU sets. The product exists only if
(c) beats or matches default tail latency without the throughput loss of (b).

**Go/no-go:** the same thread candidates must remain stable across runs, and the
policy must improve tail frames in multiple sessions rather than one benchmark.

## Product guardrails

1. Every change is transactional: snapshot, apply, monitor, restore on focus loss,
   process exit, crash recovery, and service stop.
2. Default to diagnosis. Unlock action only after the user's own trace demonstrates
   the targeted condition.
3. Never inject into anti-cheat games, install a kernel driver, alter undocumented
   scheduler state, disable security, or persist boot/device policy in version one.
4. Report “no opportunity detected” as a successful outcome.
5. Keep PresentMon/WPT capture separate from the final low-overhead policy loop where
   possible, and publish the A/B protocol plus raw CSV schema.
6. Optimize frame-time distributions, not cosmetic counters, and report regressions
   in power, temperature, average FPS, audio glitches, or network loss alongside any
   tail improvement.

## Reference index

* Microsoft: [CPU Sets](https://learn.microsoft.com/en-us/windows/win32/procthread/cpu-sets),
  [heterogeneous scheduling](https://learn.microsoft.com/en-us/windows/win32/procthread/scheduling-on-heterogeneous-processors),
  [process power throttling](https://learn.microsoft.com/en-us/windows/win32/procthread/process-power-throttling),
  [`timeBeginPeriod`](https://learn.microsoft.com/en-us/windows/win32/api/timeapi/nf-timeapi-timebeginperiod),
  [interrupt affinity](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/interrupt-affinity-and-priority),
  [D3D12 residency](https://learn.microsoft.com/en-us/windows/win32/direct3d12/residency),
  [working sets](https://learn.microsoft.com/en-us/windows/win32/memory/working-set),
  and [Wait Chain Traversal](https://learn.microsoft.com/en-us/windows/win32/debug/wait-chain-traversal).
* Microsoft performance tooling: [WPA](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/windows-performance-analyzer)
  and [GPUView](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/using-gpuview).
* Open source/vendor: [PresentMon](https://github.com/GameTechDev/PresentMon),
  [AMD ADLX](https://gpuopen.com/adlx/), and
  [NVIDIA NVML](https://developer.nvidia.com/management-library-nvml).

