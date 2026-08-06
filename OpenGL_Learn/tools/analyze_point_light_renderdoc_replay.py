"""Replay one point-light RenderDoc capture and export measured evidence.

Run this script with QRenderDoc 1.45's ``--python`` entry point. QRenderDoc
injects the RenderDoc module into the script globals on this machine; importing
it again can deadlock the portable Python runtime, so no ``import renderdoc`` is
used here.
"""

import json
import os
import pathlib
import re
import statistics
import sys
import traceback


capture_path = pathlib.Path(os.environ["POINT_LIGHT_RENDERDOC_CAPTURE"])
output_path = pathlib.Path(os.environ["POINT_LIGHT_RENDERDOC_OUTPUT"])
diagnostic_path = pathlib.Path(os.environ["POINT_LIGHT_RENDERDOC_DIAGNOSTIC"])
baseline_path = pathlib.Path(os.environ["POINT_LIGHT_RENDERDOC_BASELINE"])
coverage = os.environ["POINT_LIGHT_RENDERDOC_COVERAGE"]
light_count = int(os.environ["POINT_LIGHT_RENDERDOC_LIGHT_COUNT"])
replay_index = int(os.environ["POINT_LIGHT_RENDERDOC_REPLAY_INDEX"])
texture_path_text = os.environ.get("POINT_LIGHT_RENDERDOC_TEXTURE", "")
texture_path = pathlib.Path(texture_path_text) if texture_path_text else None

result = {
    "schemaVersion": 1,
    "success": False,
    "capture": str(capture_path),
    "output": str(output_path),
    "coverage": coverage,
    "lightCount": light_count,
    "replayIndex": replay_index,
    "replayProtocol": "independent qrenderdoc process",
    "pythonVersion": sys.version,
    "stage": "entered",
}
capture = None
controller = None


def write_result():
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(
        json.dumps(result, indent=2, sort_keys=True), encoding="utf-8"
    )


def action_dict(action):
    return {
        "eventId": int(action.eventId),
        "actionId": int(action.actionId),
        "customName": str(action.customName),
        "flags": int(action.flags),
        "numIndices": int(action.numIndices),
        "numInstances": int(action.numInstances),
        "outputs": [str(resource) for resource in list(action.outputs)],
        "depthOut": str(action.depthOut),
    }


def walk(actions):
    for action in actions:
        yield action
        for nested in walk(list(action.children)):
            yield nested


def find_marker(actions, name):
    for action in walk(actions):
        if str(action.customName) == name:
            return action
    return None


def actual_action(marker, category):
    children = list(marker.children)
    if category.startswith("stencilClear"):
        for action in children:
            if str(action.customName).startswith("glClear(Stencil"):
                return action
    else:
        for action in children:
            if int(action.numIndices) > 0:
                return action
    return None


def scalar_snapshot(value):
    snapshot = {}
    for name in dir(value):
        if name.startswith("_") or name in (
            "acquire", "append", "disown", "next", "own", "this",
            "thisown",
        ):
            continue
        try:
            child = getattr(value, name)
        except Exception:
            continue
        if callable(child):
            continue
        if isinstance(child, (bool, int, float, str)):
            snapshot[name] = child
            continue
        text = str(child)
        if "Swig Object" not in text:
            snapshot[name] = text
    return snapshot


def pipeline_snapshot(event_id):
    controller.SetFrameEvent(event_id, True)
    gl = controller.GetGLPipelineState()
    snapshot = {
        "eventId": event_id,
        "depthState": scalar_snapshot(gl.depthState),
        "stencilState": scalar_snapshot(gl.stencilState),
        "rasterizer": scalar_snapshot(gl.rasterizer),
        "rasterizerState": scalar_snapshot(gl.rasterizer.state),
        "framebuffer": scalar_snapshot(gl.framebuffer),
        "vertexShader": scalar_snapshot(gl.vertexShader),
        "fragmentShader": scalar_snapshot(gl.fragmentShader),
    }
    snapshot["stencilFrontFace"] = scalar_snapshot(gl.stencilState.frontFace)
    snapshot["stencilBackFace"] = scalar_snapshot(gl.stencilState.backFace)
    return snapshot


def message_snapshot(message):
    return scalar_snapshot(message)


def percentileless_summary(values):
    if not values:
        return {"count": 0, "sum": None, "median": None}
    return {
        "count": len(values),
        "sum": sum(values),
        "median": statistics.median(values),
        "minimum": min(values),
        "maximum": max(values),
    }


write_result()
try:
    rd = globals()["renderdoc"]
    diagnostic = json.loads(diagnostic_path.read_text(encoding="utf-8"))
    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    diagnostic_stress = diagnostic["pointLightStress"]
    baseline_stress = baseline["pointLightStress"]
    result["applicationDiagnostic"] = {
        "path": str(diagnostic_path),
        "success": bool(diagnostic["success"]),
        "sceneSignature": diagnostic_stress["sceneSignature"],
        "submissionSignature": diagnostic_stress["submissionSignature"],
        "renderDocMarkersEnabled": bool(
            diagnostic_stress["renderDocMarkersEnabled"]
        ),
        "generatedLightCount": int(diagnostic_stress["generatedLightCount"]),
        "requestedSwapInterval": int(
            diagnostic["settings"]["requestedSwapInterval"]
        ),
        "width": int(diagnostic["resolution"][0]),
        "height": int(diagnostic["resolution"][1]),
        "deferred": bool(diagnostic["settings"]["deferredRendering"]),
        "bloom": bool(diagnostic["settings"]["bloom"]),
        "ssao": bool(diagnostic["ssao"]["enabled"]),
        "pointShadowsEnabled": bool(
            diagnostic_stress["pointShadowsEnabled"]
        ),
        "pointLightStencilClearsMedian": diagnostic["profiler"]["summary"]
            ["pointLightStencilClears"]["median"],
        "pointLightsSubmittedMedian": diagnostic["profiler"]["summary"]
            ["pointLightsSubmitted"]["median"],
        "pointLightsCulledMedian": diagnostic["profiler"]["summary"]
            ["pointLightsCulled"]["median"],
    }
    result["baselineMatch"] = {
        "path": str(baseline_path),
        "sceneSignature": baseline_stress["sceneSignature"],
        "submissionSignature": baseline_stress["submissionSignature"],
        "sceneSignatureMatches": diagnostic_stress["sceneSignature"]
        == baseline_stress["sceneSignature"],
        "submissionSignatureMatches": diagnostic_stress["submissionSignature"]
        == baseline_stress["submissionSignature"],
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
    light_markers = []
    for action in list(phase.children):
        match = light_pattern.match(str(action.customName))
        if match:
            light_markers.append((int(match.group(1)), action))
    light_markers.sort(key=lambda item: item[0])

    category_marker_names = {
        "stencilClearBefore": "PointLightStress/StencilClearBefore",
        "stencilVolumeDraw": "PointLightStress/StencilVolumeDraw",
        "lightingVolumeDraw": "PointLightStress/LightingVolumeDraw",
        "stencilClearAfter": "PointLightStress/StencilClearAfter",
    }
    events = []
    event_tree_lines = [
        "{} event={} children={}".format(
            phase.customName, phase.eventId, len(list(phase.children))
        )
    ]
    for light_index, light_marker in light_markers:
        event_tree_lines.append(
            "  {} event={}".format(
                light_marker.customName, light_marker.eventId
            )
        )
        for category, marker_name in category_marker_names.items():
            marker = find_marker(list(light_marker.children), marker_name)
            if marker is None:
                raise RuntimeError(
                    "{} missing for light {}".format(marker_name, light_index)
                )
            action = actual_action(marker, category)
            if action is None:
                raise RuntimeError(
                    "{} has no classified action for light {}".format(
                        marker_name, light_index
                    )
                )
            item = action_dict(action)
            item.update(
                {
                    "lightIndex": light_index,
                    "category": category,
                    "markerEventId": int(marker.eventId),
                    "markerName": marker_name,
                }
            )
            events.append(item)
            event_tree_lines.append(
                "    {} marker={} action={} flags={} indices={}".format(
                    category,
                    marker.eventId,
                    action.eventId,
                    int(action.flags),
                    int(action.numIndices),
                )
            )

    result["eventTree"] = {
        "phase": action_dict(phase),
        "lightMarkerCount": len(light_markers),
        "classifiedEvents": events,
    }
    event_tree_path = output_path.with_suffix(".event-tree.txt")
    event_tree_path.write_text("\n".join(event_tree_lines) + "\n", encoding="utf-8")
    result["eventTreeText"] = str(event_tree_path)

    counts = {}
    for category in category_marker_names:
        counts[category] = sum(1 for event in events if event["category"] == category)
    counts["stencilClear"] = (
        counts["stencilClearBefore"] + counts["stencilClearAfter"]
    )
    counts["stencilVolumeDraw"] = counts["stencilVolumeDraw"]
    counts["lightingVolumeDraw"] = counts["lightingVolumeDraw"]
    result["eventCountValidation"] = {
        "counts": counts,
        "expected": {
            "stencilClear": 2 * light_count,
            "stencilVolumeDraw": light_count,
            "lightingVolumeDraw": light_count,
        },
        "valid": counts["stencilClear"] == 2 * light_count
        and counts["stencilVolumeDraw"] == light_count
        and counts["lightingVolumeDraw"] == light_count,
    }

    available_counters = []
    counter_by_name = {}
    for counter in list(controller.EnumerateCounters()):
        description = controller.DescribeCounter(counter)
        described = {
            "id": int(counter),
            "name": str(description.name),
            "description": str(description.description),
            "unit": str(description.unit),
            "resultType": str(description.resultType),
            "resultByteWidth": int(description.resultByteWidth),
        }
        available_counters.append(described)
        counter_by_name[described["name"]] = (counter, described)
    result["availableCounters"] = available_counters

    wanted_names = [
        "GPU Duration",
        "Samples Passed",
        "Input Vertices Read",
        "Input Primitives",
        "Rasterizer Invocations",
        "Rasterized Primitives",
        "VS Invocations",
        "PS Invocations",
    ]
    selected = [
        counter_by_name[name][0]
        for name in wanted_names
        if name in counter_by_name
    ]
    result["selectedCounters"] = [
        counter_by_name[name][1]
        for name in wanted_names
        if name in counter_by_name
    ]
    result["missingCounters"] = [
        name for name in wanted_names if name not in counter_by_name
    ]

    result["stage"] = "fetching counters"
    duration_counters = [
        counter_by_name["GPU Duration"][0]
    ] if "GPU Duration" in counter_by_name else []
    statistics_counters = [
        counter for counter in selected if int(counter) not in {
            int(item) for item in duration_counters
        }
    ]
    # Fetch timing and pipeline statistics in separate replay passes so that
    # OpenGL statistics-query instrumentation cannot perturb duration values.
    counter_results = list(controller.FetchCounters(duration_counters))
    counter_results.extend(controller.FetchCounters(statistics_counters))
    result["counterFetchPasses"] = [
        ["GPU Duration"] if duration_counters else [],
        [
            description["name"]
            for description in result["selectedCounters"]
            if description["name"] != "GPU Duration"
        ],
    ]
    event_ids = {event["eventId"] for event in events}
    values = {}
    for counter_result in counter_results:
        event_id = int(counter_result.eventId)
        if event_id not in event_ids:
            continue
        counter_id = int(counter_result.counter)
        description = next(
            item for item in result["selectedCounters"] if item["id"] == counter_id
        )
        if "Float" in description["resultType"]:
            value = float(counter_result.value.d)
        else:
            value = int(counter_result.value.u64)
        values[(event_id, counter_id)] = value

    for event in events:
        event["counters"] = {}
        for description in result["selectedCounters"]:
            value = values.get((event["eventId"], description["id"]))
            event["counters"][description["name"]] = value
        duration = event["counters"].get("GPU Duration")
        event["gpuDurationMs"] = duration * 1000.0 if duration is not None else None

    categories = {
        "stencilClear": (
            "stencilClearBefore", "stencilClearAfter"
        ),
        "stencilVolumeDraw": ("stencilVolumeDraw",),
        "lightingVolumeDraw": ("lightingVolumeDraw",),
    }
    summaries = {}
    for summary_name, category_names in categories.items():
        category_events = [
            event for event in events if event["category"] in category_names
        ]
        durations = [
            event["gpuDurationMs"]
            for event in category_events
            if event["gpuDurationMs"] is not None
        ]
        summary = {"gpuDurationMs": percentileless_summary(durations)}
        summary["counters"] = {}
        for counter_name in wanted_names[1:]:
            counter_values = [
                event["counters"].get(counter_name)
                for event in category_events
                if event["counters"].get(counter_name) is not None
            ]
            summary["counters"][counter_name] = percentileless_summary(
                counter_values
            )
        summaries[summary_name] = summary

    duration_sums = [
        summary["gpuDurationMs"]["sum"]
        for summary in summaries.values()
        if summary["gpuDurationMs"]["sum"] is not None
    ]
    classified_sum = sum(duration_sums) if len(duration_sums) == 3 else None
    for summary in summaries.values():
        category_sum = summary["gpuDurationMs"]["sum"]
        summary["pointLightPhasePercent"] = (
            category_sum * 100.0 / classified_sum
            if classified_sum and category_sum is not None
            else None
        )
    result["classSummaries"] = summaries
    result["classifiedPointLightGpuDurationMs"] = classified_sum

    first_stencil = next(
        event for event in events if event["category"] == "stencilVolumeDraw"
    )
    first_lighting = next(
        event for event in events if event["category"] == "lightingVolumeDraw"
    )
    last_lighting = next(
        event for event in reversed(events)
        if event["category"] == "lightingVolumeDraw"
    )
    result["pipelineState"] = {
        "stencilVolumeDraw": pipeline_snapshot(first_stencil["eventId"]),
        "lightingVolumeDraw": pipeline_snapshot(first_lighting["eventId"]),
    }

    if texture_path is not None:
        texture_path.parent.mkdir(parents=True, exist_ok=True)
        controller.SetFrameEvent(last_lighting["eventId"], True)
        texture_save = rd.TextureSave()
        texture_save.resourceId = list(
            next(
                action for action in walk(roots)
                if int(action.eventId) == last_lighting["eventId"]
            ).outputs
        )[0]
        texture_save.destType = rd.FileType.PNG
        save_result = controller.SaveTexture(texture_save, str(texture_path))
        result["textureViewerEvidence"] = {
            "path": str(texture_path),
            "eventId": last_lighting["eventId"],
            "resourceId": str(texture_save.resourceId),
            "saveResult": str(save_result),
            "exists": texture_path.exists(),
            "bytes": texture_path.stat().st_size if texture_path.exists() else 0,
        }

    debug_messages = list(controller.GetDebugMessages())
    result["debugMessages"] = [message_snapshot(message) for message in debug_messages]
    result["fatalReplayStatusAfter"] = str(controller.GetFatalErrorStatus())
    result["validation"] = {
        "captureOpenSucceeded": "Success" in result["captureOpenResult"],
        "captureReplaySucceeded": "Success" in result["captureReplayResult"],
        "noFatalReplayError": "Success" in result["fatalReplayStatusAfter"],
        "eventCountsValid": result["eventCountValidation"]["valid"],
        "sceneSignatureMatchesBaseline": result["baselineMatch"]
            ["sceneSignatureMatches"],
        "submissionSignatureMatchesBaseline": result["baselineMatch"]
            ["submissionSignatureMatches"],
        "fixedConfigurationValid": result["applicationDiagnostic"]["width"] == 1920
        and result["applicationDiagnostic"]["height"] == 1080
        and result["applicationDiagnostic"]["requestedSwapInterval"] == 0
        and result["applicationDiagnostic"]["deferred"]
        and not result["applicationDiagnostic"]["bloom"]
        and not result["applicationDiagnostic"]["ssao"]
        and not result["applicationDiagnostic"]["pointShadowsEnabled"],
    }
    result["success"] = all(result["validation"].values())
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
