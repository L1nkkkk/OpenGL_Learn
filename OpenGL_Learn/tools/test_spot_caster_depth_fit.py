#!/usr/bin/env python3
"""CPU-only checks for conservative Spot OBB depth fitting."""

from __future__ import annotations

import math
from dataclasses import dataclass


Vec3 = tuple[float, float, float]


def add(left: Vec3, right: Vec3) -> Vec3:
    return tuple(a + b for a, b in zip(left, right))


def scale(value: Vec3, scalar: float) -> Vec3:
    return tuple(component * scalar for component in value)


def dot(left: Vec3, right: Vec3) -> float:
    return sum(a * b for a, b in zip(left, right))


def length(value: Vec3) -> float:
    return math.sqrt(dot(value, value))


def normalize(value: Vec3) -> Vec3:
    magnitude = length(value)
    if magnitude <= 1e-12:
        raise ValueError("cannot normalize a zero vector")
    return scale(value, 1.0 / magnitude)


def cross(left: Vec3, right: Vec3) -> Vec3:
    return (
        left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0],
    )


def shadow_basis(direction: Vec3) -> tuple[Vec3, Vec3, Vec3]:
    forward = (
        normalize(direction)
        if length(direction) > 0.0001
        else (0.0, -1.0, 0.0)
    )
    usable_up = (0.0, 1.0, 0.0)
    if abs(dot(forward, usable_up)) > 0.999:
        usable_up = (1.0, 0.0, 0.0)
    right = normalize(cross(forward, usable_up))
    up = normalize(cross(right, forward))
    return forward, right, up


@dataclass(frozen=True)
class Obb:
    center: Vec3
    axes: tuple[Vec3, Vec3, Vec3]

    def support(self, normal: Vec3) -> float:
        return sum(abs(dot(normal, axis)) for axis in self.axes)


def classify(
    obb: Obb,
    light_position: Vec3,
    direction: Vec3,
    half_angle_degrees: float,
) -> tuple[bool, float, float]:
    forward, right, up = shadow_basis(direction)
    angle = math.radians(max(0.5, min(87.5, half_angle_degrees)))
    sine = math.sin(angle)
    cosine = math.cos(angle)
    planes = (
        add(scale(forward, sine), scale(right, cosine)),
        add(scale(forward, sine), scale(right, -cosine)),
        add(scale(forward, sine), scale(up, cosine)),
        add(scale(forward, sine), scale(up, -cosine)),
    )
    to_center = tuple(
        center - light
        for center, light in zip(obb.center, light_position)
    )
    center_depth = dot(forward, to_center)
    forward_support = obb.support(forward)
    epsilon = max(
        0.001,
        1e-5
        * (
            length(to_center)
            + sum(length(axis) for axis in obb.axes)
        ),
    )
    if center_depth + forward_support < -epsilon:
        return False, center_depth - forward_support, center_depth + forward_support
    for plane in planes:
        if dot(plane, to_center) + obb.support(plane) < -epsilon:
            return False, center_depth - forward_support, center_depth + forward_support
    return True, center_depth - forward_support, center_depth + forward_support


def fit_range(minimum_depth: float, maximum_depth: float) -> tuple[float, float]:
    positive_minimum = max(0.05, minimum_depth)
    span = max(0.1, maximum_depth - positive_minimum)
    near_margin = max(0.01, span * 0.01)
    far_margin = max(0.05, span * 0.02)
    near_plane = max(0.05, minimum_depth - near_margin)
    far_plane = max(near_plane + 0.1, maximum_depth + far_margin)
    return near_plane, far_plane


def projection_depth_scale(near_plane: float, far_plane: float) -> float:
    return far_plane * near_plane / (far_plane - near_plane)


def obb_is_finite(obb: Obb) -> bool:
    return all(
        math.isfinite(component)
        for vector in (obb.center, *obb.axes)
        for component in vector
    )


def improves_legacy_range(
    fitted: tuple[float, float],
    legacy: tuple[float, float],
) -> bool:
    fitted_span = fitted[1] - fitted[0]
    legacy_span = legacy[1] - legacy[0]
    return fitted_span + 0.001 < legacy_span


def main() -> None:
    forward, right, up = shadow_basis((0.0, -1.0, 0.0))
    assert math.isclose(length(forward), 1.0)
    assert math.isclose(length(right), 1.0)
    assert math.isclose(length(up), 1.0)
    assert abs(dot(forward, right)) < 1e-12
    assert abs(dot(forward, up)) < 1e-12
    assert abs(dot(right, up)) < 1e-12

    # A square perspective projection includes its corners even when the same
    # point lies outside an inscribed circular cone.
    angle = math.radians(35.0)
    corner = Obb(
        center=(0.9 * 10.0 * math.tan(angle),) * 2 + (10.0,),
        axes=((0.01, 0.0, 0.0), (0.0, 0.01, 0.0), (0.0, 0.0, 0.01)),
    )
    accepted, _, _ = classify(corner, (0.0, 0.0, 0.0), (0.0, 0.0, 1.0), 35.0)
    assert accepted
    radial_distance = math.hypot(corner.center[0], corner.center[1])
    assert radial_distance > corner.center[2] * math.tan(angle)

    # The OBB center can lie outside a side plane while its extent still
    # intersects the projection. A center-only test would incorrectly reject.
    crossing = Obb(
        center=(7.3, 0.0, 10.0),
        axes=((0.5, 0.0, 0.0), (0.0, 0.1, 0.0), (0.0, 0.0, 0.1)),
    )
    accepted, _, _ = classify(crossing, (0.0, 0.0, 0.0), (0.0, 0.0, 1.0), 35.0)
    assert accepted

    behind = Obb(
        center=(0.0, 0.0, -2.0),
        axes=((0.2, 0.0, 0.0), (0.0, 0.2, 0.0), (0.0, 0.0, 0.2)),
    )
    accepted, _, _ = classify(behind, (0.0, 0.0, 0.0), (0.0, 0.0, 1.0), 35.0)
    assert not accepted
    assert not any(
        classify(
            Obb(
                center=(offset, 0.0, -2.0),
                axes=behind.axes,
            ),
            (0.0, 0.0, 0.0),
            (0.0, 0.0, 1.0),
            35.0,
        )[0]
        for offset in (-10.0, 0.0, 10.0)
    )

    sheared = Obb(
        center=(7.4, 0.0, 10.0),
        axes=((0.6, 0.2, 0.0), (0.1, 0.6, 0.0), (0.0, 0.1, 0.2)),
    )
    assert classify(
        sheared,
        (0.0, 0.0, 0.0),
        (0.0, 0.0, 1.0),
        35.0,
    )[0]

    invalid = Obb(
        center=(0.0, 0.0, 1.0),
        axes=((math.nan, 0.0, 0.0), (0.0, 0.1, 0.0), (0.0, 0.0, 0.1)),
    )
    assert not obb_is_finite(invalid)

    near_obb = Obb(
        center=(0.0, 0.0, 3.0),
        axes=((0.5, 0.0, 0.0), (0.0, 0.5, 0.0), (0.0, 0.0, 0.5)),
    )
    far_obb = Obb(
        center=(0.0, 0.0, 12.0),
        axes=((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)),
    )
    intervals = [
        classify(item, (0.0, 0.0, 0.0), (0.0, 0.0, 1.0), 35.0)[1:]
        for item in (near_obb, far_obb)
    ]
    minimum_depth = min(interval[0] for interval in intervals)
    maximum_depth = max(interval[1] for interval in intervals)
    near_plane, far_plane = fit_range(minimum_depth, maximum_depth)
    assert near_plane <= minimum_depth
    assert far_plane >= maximum_depth
    assert near_plane >= 0.05
    assert far_plane > near_plane + 0.1

    legacy_near, legacy_far = 0.05, 25.0
    assert far_plane - near_plane < legacy_far - legacy_near
    assert (
        projection_depth_scale(near_plane, far_plane)
        / projection_depth_scale(legacy_near, legacy_far)
        > 10.0
    )
    assert improves_legacy_range(
        (near_plane, far_plane),
        (legacy_near, legacy_far),
    )

    crossing_near = Obb(
        center=(0.0, 0.0, 0.03),
        axes=((0.02, 0.0, 0.0), (0.0, 0.02, 0.0), (0.0, 0.0, 0.08)),
    )
    accepted, raw_near, raw_far = classify(
        crossing_near,
        (0.0, 0.0, 0.0),
        (0.0, 0.0, 1.0),
        35.0,
    )
    assert accepted and raw_near < 0.05
    clipped_near, clipped_far = fit_range(raw_near, raw_far)
    assert clipped_near == 0.05
    assert clipped_far >= raw_far

    # A candidate that cannot reduce the old span must take the fail-open
    # route instead of paying for a different range with no benefit.
    no_improvement = fit_range(0.05, 24.99)
    assert not improves_legacy_range(
        no_improvement,
        (legacy_near, legacy_far),
    )

    print(
        "Spot caster-depth-fit math passed: square corners, crossing and "
        "sheared OBBs are retained; behind/invalid inputs and non-improving "
        "ranges exercise conservative rejection or fallback; projected depth "
        "intervals remain covered."
    )


if __name__ == "__main__":
    main()
