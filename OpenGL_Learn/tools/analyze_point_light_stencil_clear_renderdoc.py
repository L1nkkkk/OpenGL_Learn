"""Validate one Coalesced N+1 point-light capture in QRenderDoc 1.45."""

import json
import os
import pathlib
import re
import sys
import traceback


capture_path = pathlib.Path(os.environ["POINT_LIGHT_CLEAR_RDC"])
output_path = pathlib.Path(os.environ["POINT_LIGHT_CLEAR_RDC_OUTPUT"])
diagnostic_path = pathlib.Path(os.environ["POINT_LIGHT_CLEAR_DIAGNOSTIC"])
light_count = int(os.environ.get("POINT_LIGHT_CLEAR_COUNT", "512"))
result = {
    "schemaVersion": 1,
    "success": False,
    "capture": str(capture_path),
    "diagnostic": str(diagnostic_path),
    "lightCount": light_count,
    "mode": "coalesced-n-plus-one",
    "replayProtocol": "independent qrenderdoc process; event-count validation only",
    "pythonVersion": sys.version,
    "stage": "entered",
}
capture = None
controller = None


def write_result():
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(result, indent=2, sort_keys=True), encoding="utf-8")


def walk(actions):
    for action in actions:
        yield action
        for child in walk(list(action.children)):
            yield child


def find_marker(actions, name):
    for action in walk(actions):
        if str(action.customName) == name:
            return action
    return None


def find_actual(marker, draw):
    for action in walk(list(marker.children)):
        if draw and int(action.numIndices) > 0:
            return action
        if not draw and str(action.customName).startswith("glClear(Stencil"):
            return action
    return None


def action_dict(action):
    return {
        "eventId": int(action.eventId),
        "actionId": int(action.actionId),
        "customName": str(action.customName),
        "flags": int(action.flags),
        "numIndices": int(action.numIndices),
        "numInstances": int(action.numInstances),
    }


def scalar_snapshot(value):
    snapshot = {}
    for name in dir(value):
        if name.startswith("_") or name in ("acquire", "append", "disown", "next", "own", "this", "thisown"):
            continue
        try:
            child = getattr(value, name)
        except Exception:
            continue
        if callable(child):
            continue
        if isinstance(child, (bool, int, float, str)):
            snapshot[name] = child
    return snapshot


write_result()
try:
    rd = globals()["renderdoc"]
    diagnostic = json.loads(diagnostic_path.read_text(encoding="utf-8-sig"))
    point = diagnostic["pointLightStress"]
    summary = diagnostic["profiler"]["summary"]
    result["applicationDiagnostic"] = {
        "success": bool(diagnostic["success"]),
        "schemaVersion": int(diagnostic["schemaVersion"]),
        "width": int(diagnostic["resolution"][0]),
        "height": int(diagnostic["resolution"][1]),
        "mode": str(point["stencilClearMode"]),
        "modeExplicit": bool(point["stencilClearModeExplicit"]),
        "sceneSignature": str(point["sceneSignature"]),
        "submissionSignature": str(point["submissionSignature"]),
        "generatedLightCount": int(point["generatedLightCount"]),
        "pointLightStencilClearsMedian": float(summary["pointLightStencilClears"]["median"]),
        "fixedStencilClearsMedian": float(summary["fixedStencilClears"]["median"]),
        "pointLightsSubmittedMedian": float(summary["pointLightsSubmitted"]["median"]),
        "pointLightsCulledMedian": float(summary["pointLightsCulled"]["median"]),
        "drawCallsMedian": float(summary["drawCalls"]["median"]),
        "requestedSwapInterval": int(diagnostic["settings"]["requestedSwapInterval"]),
        "deferred": bool(diagnostic["settings"]["deferredRendering"]),
        "bloom": bool(diagnostic["settings"]["bloom"]),
        "ssao": bool(diagnostic["ssao"]["enabled"]),
        "pointShadows": bool(point["pointShadowsEnabled"]),
    }

    capture = rd.OpenCaptureFile()
    result["stage"] = "opening capture"
    open_result = capture.OpenFile(str(capture_path), "", None)
    result["captureOpenResult"] = str(open_result)
    result["localReplaySupport"] = bool(capture.LocalReplaySupport())
    replay_result, controller = capture.OpenCapture(rd.ReplayOptions(), None)
    result["captureReplayResult"] = str(replay_result)
    result["fatalReplayStatusBefore"] = str(controller.GetFatalErrorStatus())

    roots = list(controller.GetRootActions())
    phase = find_marker(roots, "PointLightStress/PointLightPhase")
    if phase is None:
        raise RuntimeError("PointLightStress/PointLightPhase marker is missing")
    light_pattern = re.compile(r"^PointLightStress/Light\[(\d{4})\]$")
    lights = []
    for action in list(phase.children):
        match = light_pattern.match(str(action.customName))
        if match:
            lights.append((int(match.group(1)), action))
    lights.sort(key=lambda item: item[0])

    counts = {
        "lightMarkers": len(lights),
        "stencilClearInitial": 0,
        "stencilClearBefore": 0,
        "stencilClearAfter": 0,
        "stencilVolumeDraw": 0,
        "lightingVolumeDraw": 0,
    }
    tree_lines = [
        "{} event={} children={}".format(phase.customName, phase.eventId, len(list(phase.children)))
    ]
    samples = []
    for light_index, light in lights:
        child_names = [str(action.customName) for action in walk(list(light.children))]
        initial = find_marker(list(light.children), "PointLightStress/StencilClearInitial")
        before = find_marker(list(light.children), "PointLightStress/StencilClearBefore")
        after = find_marker(list(light.children), "PointLightStress/StencilClearAfter")
        stencil_draw = find_marker(list(light.children), "PointLightStress/StencilVolumeDraw")
        lighting_draw = find_marker(list(light.children), "PointLightStress/LightingVolumeDraw")
        if initial is not None:
            if find_actual(initial, False) is None:
                raise RuntimeError("initial marker has no stencil clear")
            counts["stencilClearInitial"] += 1
        if before is not None:
            if find_actual(before, False) is None:
                raise RuntimeError("before marker has no stencil clear")
            counts["stencilClearBefore"] += 1
        if after is None or find_actual(after, False) is None:
            raise RuntimeError("light {} is missing ClearAfter".format(light_index))
        if stencil_draw is None or find_actual(stencil_draw, True) is None:
            raise RuntimeError("light {} is missing stencil draw".format(light_index))
        if lighting_draw is None or find_actual(lighting_draw, True) is None:
            raise RuntimeError("light {} is missing lighting draw".format(light_index))
        counts["stencilClearAfter"] += 1
        counts["stencilVolumeDraw"] += 1
        counts["lightingVolumeDraw"] += 1
        if light_index in (0, light_count - 1):
            samples.append({
                "lightIndex": light_index,
                "marker": action_dict(light),
                "childMarkerNames": child_names,
                "initial": action_dict(initial) if initial is not None else None,
                "after": action_dict(after),
                "stencilDraw": action_dict(find_actual(stencil_draw, True)),
                "lightingDraw": action_dict(find_actual(lighting_draw, True)),
            })
        tree_lines.append(
            "  light={:04d} marker={} initial={} stencilDraw={} lightingDraw={} after={}".format(
                light_index,
                light.eventId,
                initial.eventId if initial is not None else "-",
                stencil_draw.eventId,
                lighting_draw.eventId,
                after.eventId,
            )
        )

    counts["pointLightStencilClears"] = (
        counts["stencilClearInitial"]
        + counts["stencilClearBefore"]
        + counts["stencilClearAfter"]
    )
    expected = {
        "lightMarkers": light_count,
        "stencilClearInitial": 1 if light_count > 0 else 0,
        "stencilClearBefore": 0,
        "stencilClearAfter": light_count,
        "pointLightStencilClears": light_count + 1 if light_count > 0 else 0,
        "stencilVolumeDraw": light_count,
        "lightingVolumeDraw": light_count,
    }
    event_counts_valid = all(counts[name] == value for name, value in expected.items())
    tree_path = output_path.with_suffix(".event-tree.txt")
    tree_path.write_text("\n".join(tree_lines) + "\n", encoding="utf-8")
    result["eventTree"] = {
        "phase": action_dict(phase),
        "counts": counts,
        "expected": expected,
        "valid": event_counts_valid,
        "firstAndLastLightSamples": samples,
        "textPath": str(tree_path),
    }
    result["debugMessages"] = [scalar_snapshot(message) for message in list(controller.GetDebugMessages())]
    result["fatalReplayStatusAfter"] = str(controller.GetFatalErrorStatus())
    app = result["applicationDiagnostic"]
    result["validation"] = {
        "captureOpenSucceeded": "Success" in result["captureOpenResult"],
        "captureReplaySucceeded": "Success" in result["captureReplayResult"],
        "noFatalReplayError": "Success" in result["fatalReplayStatusAfter"],
        "eventCountsValid": event_counts_valid,
        "applicationModeValid": app["mode"] == "coalesced-n-plus-one" and app["modeExplicit"],
        "applicationCountersValid": app["generatedLightCount"] == light_count
        and app["pointLightsSubmittedMedian"] == light_count
        and app["pointLightsCulledMedian"] == 0
        and app["pointLightStencilClearsMedian"] == expected["pointLightStencilClears"]
        and app["fixedStencilClearsMedian"] == 3,
        "fixedConfigurationValid": app["width"] == 1920
        and app["height"] == 1080
        and app["requestedSwapInterval"] == 0
        and app["deferred"]
        and not app["bloom"]
        and not app["ssao"]
        and not app["pointShadows"],
    }
    result["success"] = bool(app["success"]) and all(result["validation"].values())
    result["stage"] = "complete"
except BaseException as error:
    result["error"] = repr(error)
    result["traceback"] = traceback.format_exc()
finally:
    if controller is not None:
        controller.Shutdown()
    if capture is not None:
        capture.Shutdown()
    write_result()

sys.exit(0 if result["success"] else 2)
