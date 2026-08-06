"""Validate one fixed Analytic Screen point-light capture in QRenderDoc 1.45."""

import json
import os
import pathlib
import re
import sys
import traceback


capture_path = pathlib.Path(os.environ["POINT_LIGHT_SCREEN_RDC"])
output_path = pathlib.Path(os.environ["POINT_LIGHT_SCREEN_RDC_OUTPUT"])
diagnostic_path = pathlib.Path(os.environ["POINT_LIGHT_SCREEN_DIAGNOSTIC"])
light_count = int(os.environ.get("POINT_LIGHT_SCREEN_COUNT", "512"))
result = {
    "schemaVersion": 1,
    "success": False,
    "capture": str(capture_path),
    "diagnostic": str(diagnostic_path),
    "lightCount": light_count,
    "mode": "analytic-screen",
    "replayProtocol": "independent qrenderdoc process; event-count and debug-message validation only",
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


def find_draw(marker):
    if marker is None:
        return None
    for action in walk(list(marker.children)):
        if int(action.numIndices) > 0:
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
        "renderMode": str(point["renderMode"]),
        "renderModeExplicit": bool(point["renderModeExplicit"]),
        "stencilClearMode": str(point["stencilClearMode"]),
        "stencilClearModeExplicit": bool(point["stencilClearModeExplicit"]),
        "offscreenCulling": bool(point["offscreenCulling"]),
        "offscreenCullingExplicit": bool(point["offscreenCullingExplicit"]),
        "sceneSignature": str(point["sceneSignature"]),
        "submissionSignature": str(point["submissionSignature"]),
        "generatedLightCount": int(point["generatedLightCount"]),
        "pointLightStencilClearsMedian": float(summary["pointLightStencilClears"]["median"]),
        "fixedStencilClearsMedian": float(summary["fixedStencilClears"]["median"]),
        "pointLightsSubmittedMedian": float(summary["pointLightsSubmitted"]["median"]),
        "pointLightsCulledMedian": float(summary["pointLightsCulled"]["median"]),
        "screenDrawsMedian": float(summary["pointLightScreenDraws"]["median"]),
        "stencilDrawsMedian": float(summary["pointLightStencilDraws"]["median"]),
        "lightingVolumeDrawsMedian": float(summary["pointLightLightingVolumeDraws"]["median"]),
        "requestedSwapInterval": int(diagnostic["settings"]["requestedSwapInterval"]),
        "deferred": bool(diagnostic["settings"]["deferredRendering"]),
        "bloom": bool(diagnostic["settings"]["bloom"]),
        "ssao": bool(diagnostic["ssao"]["enabled"]),
        "pointShadows": bool(point["pointShadowsEnabled"]),
        "stencilExitClean": bool(point["stencilLifecycleValidation"]["clean"]),
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
        "lightingScreenDraw": 0,
        "stencilVolumeDraw": 0,
        "lightingVolumeDraw": 0,
        "pointLightStencilClears": 0,
    }
    tree_lines = [
        "{} event={} children={}".format(phase.customName, phase.eventId, len(list(phase.children)))
    ]
    samples = []
    for action in walk(list(phase.children)):
        name = str(action.customName)
        if name == "PointLightStress/StencilVolumeDraw":
            counts["stencilVolumeDraw"] += 1
        elif name == "PointLightStress/LightingVolumeDraw":
            counts["lightingVolumeDraw"] += 1
        elif name.startswith("glClear(Stencil"):
            counts["pointLightStencilClears"] += 1

    for light_index, light in lights:
        screen_marker = find_marker(list(light.children), "PointLightStress/LightingScreenDraw")
        screen_draw = find_draw(screen_marker)
        if screen_marker is None or screen_draw is None:
            raise RuntimeError("light {} is missing screen draw".format(light_index))
        counts["lightingScreenDraw"] += 1
        if light_index in (0, light_count - 1):
            samples.append(
                {
                    "lightIndex": light_index,
                    "marker": action_dict(light),
                    "screenDraw": action_dict(screen_draw),
                }
            )
        tree_lines.append(
            "  light={:04d} marker={} screenDraw={}".format(
                light_index, light.eventId, screen_marker.eventId
            )
        )

    expected = {
        "lightMarkers": light_count,
        "lightingScreenDraw": light_count,
        "stencilVolumeDraw": 0,
        "lightingVolumeDraw": 0,
        "pointLightStencilClears": 0,
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
    debug_messages = [scalar_snapshot(message) for message in list(controller.GetDebugMessages())]
    result["debugMessages"] = debug_messages
    result["fatalReplayStatusAfter"] = str(controller.GetFatalErrorStatus())
    app = result["applicationDiagnostic"]
    result["validation"] = {
        "captureOpenSucceeded": "Success" in result["captureOpenResult"],
        "captureReplaySucceeded": "Success" in result["captureReplayResult"],
        "noFatalReplayError": "Success" in result["fatalReplayStatusAfter"],
        "noReplayDebugMessages": len(debug_messages) == 0,
        "eventCountsValid": event_counts_valid,
        "applicationModeValid": app["renderMode"] == "analytic-screen"
        and app["renderModeExplicit"]
        and app["stencilClearMode"] == "coalesced-n-plus-one"
        and app["stencilClearModeExplicit"]
        and not app["offscreenCulling"]
        and app["offscreenCullingExplicit"],
        "applicationCountersValid": app["generatedLightCount"] == light_count
        and app["pointLightsSubmittedMedian"] == light_count
        and app["pointLightsCulledMedian"] == 0
        and app["pointLightStencilClearsMedian"] == 0
        and app["fixedStencilClearsMedian"] == 3
        and app["screenDrawsMedian"] == light_count
        and app["stencilDrawsMedian"] == 0
        and app["lightingVolumeDrawsMedian"] == 0,
        "fixedConfigurationValid": app["width"] == 1920
        and app["height"] == 1080
        and app["requestedSwapInterval"] == 0
        and app["deferred"]
        and not app["bloom"]
        and not app["ssao"]
        and not app["pointShadows"],
        "stencilExitClean": app["stencilExitClean"],
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
