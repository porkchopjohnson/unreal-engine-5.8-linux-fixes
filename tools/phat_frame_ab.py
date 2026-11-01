"""Run via the Unreal Python console with the intended PhAT workload already visible."""

import json
from pathlib import Path
import statistics
import tempfile
import time

import unreal


_ab_modes = [1, 0, 0, 1]
_ab_phase = 0
_ab_times = []
_ab_results = []
_ab_start = time.perf_counter()
_ab_output = Path(tempfile.gettempdir()) / "ue583-phat-frame-ab.json"


def _ab_mode(mode):
    unreal.SystemLibrary.execute_console_command(
        None, "r.Vulkan.DeferredDeletionQueue.LinearValidation %d" % mode
    )


def _ab_tick(dt):
    global _ab_start, _ab_phase, _ab_times
    now = time.perf_counter()
    _ab_times.append(now)
    if now - _ab_start >= 30:
        data = [
            (b - a) * 1000
            for a, b in zip(_ab_times, _ab_times[1:])
            if a - _ab_start >= 10
        ]
        _ab_results.append(
            dict(phase=_ab_phase, linear_crosscheck=_ab_modes[_ab_phase], intervals_ms=data)
        )
        unreal.log(
            "PhAT AB phase %d linear_crosscheck=%d n=%d median_ms=%.3f mean_ms=%.3f"
            % (
                _ab_phase,
                _ab_modes[_ab_phase],
                len(data),
                statistics.median(data),
                statistics.mean(data),
            )
        )
        _ab_phase += 1
        _ab_times = []
        _ab_start = now
        if _ab_phase == len(_ab_modes):
            unreal.unregister_slate_post_tick_callback(_ab_callback)
            _ab_mode(0)
            _ab_output.write_text(json.dumps(_ab_results), encoding="utf-8")
            unreal.log("PhAT AB finished; hash-only validation restored; output: %s" % _ab_output)
        else:
            _ab_mode(_ab_modes[_ab_phase])


_ab_mode(_ab_modes[0])
_ab_callback = unreal.register_slate_post_tick_callback(_ab_tick)
unreal.log("PhAT AB started: four 30s phases, first 10s of each discarded")
