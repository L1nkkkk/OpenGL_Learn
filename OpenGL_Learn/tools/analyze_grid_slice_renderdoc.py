"""QRenderDoc-side validation for a representative middle-S grid draw."""

import json
import os
import pathlib
import sys
import traceback


capture_path = pathlib.Path(os.environ["GRID_SLICE_RDC"])
output_path = pathlib.Path(os.environ["GRID_SLICE_RDC_OUTPUT"])
diagnostic_path = pathlib.Path(os.environ["GRID_SLICE_DIAGNOSTIC"])
expected_slices = int(os.environ["GRID_SLICE_EXPECTED"])
result = {
    "schemaVersion": 1,
    "success": False,
    "capture": str(capture_path),
    "diagnostic": str(diagnostic_path),
    "expectedSliceCount": expected_slices,
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


def find_shader_variable(variables, target):
    for variable in variables:
        if str(variable.name) == target:
            return variable
        found = find_shader_variable(list(variable.members), target)
        if found is not None:
            return found
    return None


def shader_variable_int(variable):
    value = variable.value
    for field in ("s32v", "u32v", "s64v", "u64v"):
        try:
            values = list(getattr(value, field))
            if values:
                return int(values[0])
        except Exception:
            pass
    raise RuntimeError("gridSliceCount was reflected but its integer value could not be read")


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
        "sliceCountConfigured": int(point["gridSliceCountConfigured"]),
        "sliceCountExplicit": bool(point["gridSliceCountExplicit"]),
        "sliceCountRuntime": int(grid["sliceCount"]),
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
    marker_name = "PointLightStress/GridS{}LightingDraw".format(expected_slices)
    grid_marker = find_named(list(phase.children) if phase else [], marker_name)
    grid_draw = find_draw(grid_marker)
    if phase is None or grid_marker is None or grid_draw is None:
        raise RuntimeError("point-light phase / expected grid draw marker is missing")

    counts = {
        "matchingGridMarkers": 0,
        "allGridMarkers": 0,
        "legacyScreenMarkers": 0,
        "stencilVolumeMarkers": 0,
        "lightingVolumeMarkers": 0,
        "drawActionsInsidePointLightPhase": 0,
    }
    for action in walk(list(phase.children)):
        name = str(action.customName)
        if name == marker_name:
            counts["matchingGridMarkers"] += 1
        if name.startswith("PointLightStress/GridS") and name.endswith("LightingDraw"):
            counts["allGridMarkers"] += 1
        elif name == "PointLightStress/LightingScreenDraw":
            counts["legacyScreenMarkers"] += 1
        elif name == "PointLightStress/StencilVolumeDraw":
            counts["stencilVolumeMarkers"] += 1
        elif name == "PointLightStress/LightingVolumeDraw":
            counts["lightingVolumeMarkers"] += 1
        if int(action.numIndices) > 0:
            counts["drawActionsInsidePointLightPhase"] += 1

    controller.SetFrameEvent(int(grid_draw.eventId), True)
    stage = rd.ShaderStage.Fragment
    pipeline = controller.GetPipelineState()
    reflection = pipeline.GetShaderReflection(stage)
    if reflection is None:
        raise RuntimeError("fragment shader reflection is unavailable")

    reflected_read_only = [scalar_snapshot(resource) for resource in list(reflection.readOnlyResources)]
    bound_arrays = []
    buffer_texture_count = 0
    textures_by_id = {str(texture.resourceId): texture for texture in list(controller.GetTextures())}
    for used in list(pipeline.GetReadOnlyResources(stage)):
        record = {"usedDescriptor": scalar_snapshot(used)}
        descriptor = getattr(used, "descriptor", None)
        if descriptor is not None:
            record["descriptor"] = scalar_snapshot(descriptor)
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
                record["texture"] = scalar_snapshot(texture)
                if "Buffer" in str(getattr(texture, "type", "")):
                    buffer_texture_count += 1
        bound_arrays.append(record)

    constant_blocks = []
    uniform_variable = None
    for index, block in enumerate(list(reflection.constantBlocks)):
        block_record = {"index": index, "reflection": scalar_snapshot(block)}
        used = pipeline.GetConstantBlock(stage, index, 0)
        descriptor = getattr(used, "descriptor", None)
        block_record["binding"] = scalar_snapshot(used)
        if descriptor is not None:
            block_record["descriptor"] = scalar_snapshot(descriptor)
        resource = getattr(descriptor, "resource", rd.ResourceId.Null()) if descriptor is not None else rd.ResourceId.Null()
        offset = int(getattr(descriptor, "byteOffset", 0)) if descriptor is not None else 0
        length = int(getattr(descriptor, "byteSize", 0)) if descriptor is not None else 0
        variables = list(controller.GetCBufferVariableContents(
            pipeline.GetGraphicsPipelineObject(),
            reflection.resourceId,
            stage,
            pipeline.GetShaderEntryPoint(stage),
            index,
            resource,
            offset,
            length,
        ))
        block_record["variableNames"] = [str(variable.name) for variable in variables]
        constant_blocks.append(block_record)
        candidate = find_shader_variable(variables, "gridSliceCount")
        if candidate is not None:
            uniform_variable = candidate

    if uniform_variable is None:
        raise RuntimeError("gridSliceCount was not found in captured fragment uniforms")
    captured_slice_count = shader_variable_int(uniform_variable)
    reflected_names = [str(item.get("name", "")) for item in reflected_read_only]
    required_names = ("gridMetadata", "gridIndices", "gridLights")
    reflected_grid_resources = [name for name in required_names if name in reflected_names]
    debug_messages = [scalar_snapshot(message) for message in list(controller.GetDebugMessages())]
    fatal = str(controller.GetFatalErrorStatus())
    result["eventTree"] = {
        "phase": action_dict(phase),
        "gridMarker": action_dict(grid_marker),
        "gridDraw": action_dict(grid_draw),
        "counts": counts,
    }
    result["fragmentResources"] = {
        "reflection": reflected_read_only,
        "boundArrays": bound_arrays,
        "requiredGridResources": list(required_names),
        "reflectedGridResources": reflected_grid_resources,
        "bufferTextureBindingCount": buffer_texture_count,
    }
    result["fragmentUniforms"] = {
        "constantBlocks": constant_blocks,
        "gridSliceCount": captured_slice_count,
    }
    result["debugMessages"] = debug_messages
    result["fatalReplayStatus"] = fatal
    app = result["applicationDiagnostic"]
    result["validation"] = {
        "captureOpenSucceeded": "Success" in result["captureOpenResult"],
        "captureReplaySucceeded": "Success" in result["captureReplayResult"],
        "noFatalReplayError": "Success" in fatal,
        "noReplayDebugMessages": len(debug_messages) == 0,
        "singleGridDraw": counts["matchingGridMarkers"] == 1
        and counts["allGridMarkers"] == 1
        and counts["drawActionsInsidePointLightPhase"] == 1,
        "noLegacyOrVolumeDraw": counts["legacyScreenMarkers"] == 0
        and counts["stencilVolumeMarkers"] == 0
        and counts["lightingVolumeMarkers"] == 0,
        "threeGridTboBindings": len(reflected_grid_resources) == 3 and buffer_texture_count >= 3,
        "capturedSliceUniform": captured_slice_count == expected_slices,
        "applicationConfiguration": app["success"]
        and app["renderMode"] == "cluster16"
        and app["renderModeExplicit"]
        and app["gridUpdateMode"] == "cached"
        and app["gridUpdateModeExplicit"]
        and app["sliceCountConfigured"] == expected_slices
        and app["sliceCountExplicit"]
        and app["sliceCountRuntime"] == expected_slices
        and app["generatedLightCount"] == 512
        and abs(app["requestedRadius"] - 6.0) <= 0.0001
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
