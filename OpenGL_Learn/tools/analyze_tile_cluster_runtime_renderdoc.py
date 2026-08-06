"""QRenderDoc-side validation for the representative Cluster16 runtime draw."""

import json
import os
import pathlib
import sys
import traceback


capture_path = pathlib.Path(os.environ["TILE_CLUSTER_RUNTIME_RDC"])
output_path = pathlib.Path(os.environ["TILE_CLUSTER_RUNTIME_RDC_OUTPUT"])
diagnostic_path = pathlib.Path(os.environ["TILE_CLUSTER_RUNTIME_DIAGNOSTIC"])
result = {
    "schemaVersion": 1,
    "success": False,
    "capture": str(capture_path),
    "diagnostic": str(diagnostic_path),
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


def find_named(actions, name):
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
        elif "ResourceId" in type(child).__name__:
            snapshot[name] = str(child)
    return snapshot


def action_dict(action):
    return {
        "eventId": int(action.eventId),
        "actionId": int(action.actionId),
        "customName": str(action.customName),
        "flags": int(action.flags),
        "numIndices": int(action.numIndices),
        "numInstances": int(action.numInstances),
    }


write_result()
try:
    rd = globals()["renderdoc"]
    diagnostic = json.loads(diagnostic_path.read_text(encoding="utf-8-sig"))
    point = diagnostic["pointLightStress"]
    grid = point["gridRuntime"]
    summary = diagnostic["profiler"]["summary"]
    result["applicationDiagnostic"] = {
        "success": bool(diagnostic["success"]),
        "renderMode": str(point["renderMode"]),
        "renderModeExplicit": bool(point["renderModeExplicit"]),
        "gridUpdateMode": str(point["gridUpdateMode"]),
        "gridUpdateModeExplicit": bool(point["gridUpdateModeExplicit"]),
        "generatedLightCount": int(point["generatedLightCount"]),
        "requestedRadius": float(point["requestedRadius"]),
        "screenDrawsMedian": float(summary["pointLightScreenDraws"]["median"]),
        "stencilDrawsMedian": float(summary["pointLightStencilDraws"]["median"]),
        "volumeDrawsMedian": float(summary["pointLightLightingVolumeDraws"]["median"]),
        "gridValid": bool(grid["valid"]),
        "gridOverflow": bool(grid["overflow"]),
        "gridError": str(grid["error"]),
        "logicalCells": int(grid["logicalCells"]),
        "totalIndices": int(grid["totalIndices"]),
        "residentBytes": int(grid["residentBytes"]),
        "csrSignature": str(grid["csrSignature"]),
    }

    capture = rd.OpenCaptureFile()
    result["stage"] = "opening capture"
    open_result = capture.OpenFile(str(capture_path), "", None)
    result["captureOpenResult"] = str(open_result)
    replay_result, controller = capture.OpenCapture(rd.ReplayOptions(), None)
    result["captureReplayResult"] = str(replay_result)
    roots = list(controller.GetRootActions())
    phase = find_named(roots, "PointLightStress/PointLightPhase")
    cluster_marker = find_named(list(phase.children) if phase else [], "PointLightStress/Cluster16LightingDraw")
    cluster_draw = find_draw(cluster_marker)
    if phase is None or cluster_marker is None or cluster_draw is None:
        raise RuntimeError("point-light phase / Cluster16 draw marker is missing")

    counts = {
        "cluster16LightingMarkers": 0,
        "tile16LightingMarkers": 0,
        "legacyScreenMarkers": 0,
        "stencilVolumeMarkers": 0,
        "lightingVolumeMarkers": 0,
        "drawActionsInsidePointLightPhase": 0,
    }
    for action in walk(list(phase.children)):
        name = str(action.customName)
        if name == "PointLightStress/Cluster16LightingDraw":
            counts["cluster16LightingMarkers"] += 1
        elif name == "PointLightStress/Tile16LightingDraw":
            counts["tile16LightingMarkers"] += 1
        elif name == "PointLightStress/LightingScreenDraw":
            counts["legacyScreenMarkers"] += 1
        elif name == "PointLightStress/StencilVolumeDraw":
            counts["stencilVolumeMarkers"] += 1
        elif name == "PointLightStress/LightingVolumeDraw":
            counts["lightingVolumeMarkers"] += 1
        if int(action.numIndices) > 0:
            counts["drawActionsInsidePointLightPhase"] += 1

    controller.SetFrameEvent(int(cluster_draw.eventId), True)
    pipeline = controller.GetPipelineState()
    reflection = pipeline.GetShaderReflection(rd.ShaderStage.Fragment)
    reflected_read_only = []
    if reflection is not None:
        for resource in list(reflection.readOnlyResources):
            reflected_read_only.append(scalar_snapshot(resource))
    bound_arrays = []
    buffer_texture_count = 0
    textures_by_id = {
        str(texture.resourceId): texture for texture in list(controller.GetTextures())
    }
    for used in list(pipeline.GetReadOnlyResources(rd.ShaderStage.Fragment)):
        record = {
            "usedDescriptor": scalar_snapshot(used),
            "usedDescriptorMembers": [name for name in dir(used) if not name.startswith("_")],
        }
        descriptor = getattr(used, "descriptor", None)
        if descriptor is not None:
            record["descriptor"] = scalar_snapshot(descriptor)
            record["descriptorMembers"] = [name for name in dir(descriptor) if not name.startswith("_")]
        resource_id = None
        for owner in (descriptor, used):
            if owner is None:
                continue
            for name in ("resource", "resourceId"):
                candidate = getattr(owner, name, None)
                if candidate is not None:
                    resource_id = candidate
                    break
            if resource_id is not None:
                break
        if resource_id is not None:
            texture = textures_by_id.get(str(resource_id))
            if texture is not None:
                texture_snapshot = scalar_snapshot(texture)
                record["texture"] = texture_snapshot
                if "Buffer" in str(getattr(texture, "type", "")):
                    buffer_texture_count += 1
        bound_arrays.append(record)

    reflected_names = [str(item.get("name", "")) for item in reflected_read_only]
    required_names = ("gridMetadata", "gridIndices", "gridLights")
    reflected_grid_resources = [name for name in required_names if name in reflected_names]
    debug_messages = [scalar_snapshot(message) for message in list(controller.GetDebugMessages())]
    fatal = str(controller.GetFatalErrorStatus())
    result["eventTree"] = {
        "phase": action_dict(phase),
        "clusterMarker": action_dict(cluster_marker),
        "clusterDraw": action_dict(cluster_draw),
        "counts": counts,
    }
    result["fragmentResources"] = {
        "reflection": reflected_read_only,
        "boundArrays": bound_arrays,
        "requiredGridResources": list(required_names),
        "reflectedGridResources": reflected_grid_resources,
        "bufferTextureBindingCount": buffer_texture_count,
    }
    result["debugMessages"] = debug_messages
    result["fatalReplayStatus"] = fatal
    app = result["applicationDiagnostic"]
    result["validation"] = {
        "captureOpenSucceeded": "Success" in result["captureOpenResult"],
        "captureReplaySucceeded": "Success" in result["captureReplayResult"],
        "noFatalReplayError": "Success" in fatal,
        "noReplayDebugMessages": len(debug_messages) == 0,
        "singleClusterDraw": counts["cluster16LightingMarkers"] == 1
        and counts["drawActionsInsidePointLightPhase"] == 1,
        "noTileOrLegacyDraw": counts["tile16LightingMarkers"] == 0
        and counts["legacyScreenMarkers"] == 0
        and counts["stencilVolumeMarkers"] == 0
        and counts["lightingVolumeMarkers"] == 0,
        "threeGridTboBindings": len(reflected_grid_resources) == 3
        and buffer_texture_count >= 3,
        "applicationConfiguration": app["success"]
        and app["renderMode"] == "cluster16"
        and app["renderModeExplicit"]
        and app["gridUpdateMode"] == "cached"
        and app["gridUpdateModeExplicit"]
        and app["generatedLightCount"] == 512
        and abs(app["requestedRadius"] - 3.0) <= 0.0001
        and app["screenDrawsMedian"] == 1
        and app["stencilDrawsMedian"] == 0
        and app["volumeDrawsMedian"] == 0
        and app["gridValid"]
        and not app["gridOverflow"]
        and not app["gridError"],
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
