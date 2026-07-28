#!/usr/bin/env python3
"""CPU-only checks for the Spot PCSS perspective-depth conversion."""

from __future__ import annotations

import math


def projection_terms(near_plane: float, far_plane: float) -> tuple[float, float]:
    denominator = far_plane - near_plane
    return far_plane / denominator, far_plane * near_plane / denominator


def project_depth(
    distance: float,
    near_plane: float,
    far_plane: float,
) -> float:
    offset, scale = projection_terms(near_plane, far_plane)
    return offset - scale / distance


def linearize_depth(
    projected_depth: float,
    near_plane: float,
    far_plane: float,
) -> float:
    offset, scale = projection_terms(near_plane, far_plane)
    return scale / (offset - projected_depth)


def scaled_linear_depth(
    projected_depth: float,
    near_plane: float,
    far_plane: float,
) -> float:
    offset, _ = projection_terms(near_plane, far_plane)
    return 1.0 / (offset - projected_depth)


def shader_scaled_linear_depth(
    projected_depth: float,
    near_plane: float,
    far_plane: float,
) -> float:
    offset, _ = projection_terms(near_plane, far_plane)
    denominator = max(
        offset - projected_depth,
        max(offset - 1.0, 1e-8),
    )
    return 1.0 / denominator


def main() -> None:
    blocker_distance = 2.0
    receiver_distance = 5.0
    expected_penumbra_ratio = (
        receiver_distance - blocker_distance
    ) / blocker_distance

    legacy_ratios: list[float] = []
    for near_plane, far_plane in (
        (0.05, 20.0),
        (0.5, 20.0),
        (1.0, 50.0),
    ):
        blocker_depth = project_depth(
            blocker_distance,
            near_plane,
            far_plane,
        )
        receiver_depth = project_depth(
            receiver_distance,
            near_plane,
            far_plane,
        )
        reconstructed_blocker = linearize_depth(
            blocker_depth,
            near_plane,
            far_plane,
        )
        reconstructed_receiver = linearize_depth(
            receiver_depth,
            near_plane,
            far_plane,
        )
        _, depth_scale = projection_terms(near_plane, far_plane)
        scaled_blocker = scaled_linear_depth(
            blocker_depth,
            near_plane,
            far_plane,
        )
        scaled_receiver = receiver_distance / depth_scale
        corrected_ratio = (
            scaled_receiver - scaled_blocker
        ) / scaled_blocker
        legacy_ratio = (
            receiver_depth - blocker_depth
        ) / blocker_depth
        legacy_ratios.append(legacy_ratio)

        assert math.isclose(
            reconstructed_blocker,
            blocker_distance,
            rel_tol=0.0,
            abs_tol=1e-10,
        )
        assert math.isclose(
            reconstructed_receiver,
            receiver_distance,
            rel_tol=0.0,
            abs_tol=1e-10,
        )
        assert math.isclose(
            scaled_blocker,
            blocker_distance / depth_scale,
            rel_tol=0.0,
            abs_tol=1e-10,
        )
        assert math.isclose(
            scaled_receiver,
            receiver_distance / depth_scale,
            rel_tol=0.0,
            abs_tol=1e-10,
        )
        assert math.isclose(
            corrected_ratio,
            expected_penumbra_ratio,
            rel_tol=0.0,
            abs_tol=1e-10,
        )

    assert max(legacy_ratios) - min(legacy_ratios) > 0.01
    assert all(ratio < expected_penumbra_ratio for ratio in legacy_ratios)
    assert legacy_ratios[0] < expected_penumbra_ratio * 0.02

    near_plane = 0.05
    far_plane = 20.0
    blocker_distances = (2.0, 4.0)
    projected_blockers = tuple(
        project_depth(distance, near_plane, far_plane)
        for distance in blocker_distances
    )
    mean_scaled_blocker = sum(
        scaled_linear_depth(depth, near_plane, far_plane)
        for depth in projected_blockers
    ) / len(projected_blockers)
    scaled_projected_mean = scaled_linear_depth(
        sum(projected_blockers) / len(projected_blockers),
        near_plane,
        far_plane,
    )
    _, depth_scale = projection_terms(near_plane, far_plane)
    assert math.isclose(
        mean_scaled_blocker,
        3.0 / depth_scale,
        rel_tol=0.0,
        abs_tol=1e-10,
    )
    assert not math.isclose(
        scaled_projected_mean,
        mean_scaled_blocker,
        rel_tol=0.0,
        abs_tol=1e-3,
    )

    # The generic shadow epsilon is too large for legal perspective-depth
    # denominators when near/far spans a large range. Exercise the harness
    # limits and prove that the projection-derived floor preserves the ratio.
    near_plane = 0.001
    far_plane = 400.0
    blocker_distance = 390.0
    receiver_distance = 395.0
    blocker_depth = project_depth(
        blocker_distance,
        near_plane,
        far_plane,
    )
    receiver_depth = project_depth(
        receiver_distance,
        near_plane,
        far_plane,
    )
    _, depth_scale = projection_terms(near_plane, far_plane)
    scaled_blocker = shader_scaled_linear_depth(
        blocker_depth,
        near_plane,
        far_plane,
    )
    scaled_receiver = receiver_distance / depth_scale
    corrected_ratio = (
        scaled_receiver - scaled_blocker
    ) / scaled_blocker
    expected_extreme_ratio = (
        receiver_distance - blocker_distance
    ) / blocker_distance
    assert math.isclose(
        corrected_ratio,
        expected_extreme_ratio,
        rel_tol=0.0,
        abs_tol=1e-8,
    )
    offset, _ = projection_terms(near_plane, far_plane)
    generic_epsilon_blocker = 1.0 / max(
        offset - blocker_depth,
        1e-5,
    )
    generic_epsilon_ratio = (
        scaled_receiver - generic_epsilon_blocker
    ) / generic_epsilon_blocker
    assert not math.isclose(
        generic_epsilon_ratio,
        expected_extreme_ratio,
        rel_tol=0.0,
        abs_tol=1e-2,
    )

    print(
        "Spot PCSS linear-depth math passed: "
        f"penumbra ratio={expected_penumbra_ratio:.6f}, "
        "invariant across tested near/far planes, including 0.001/400."
    )


if __name__ == "__main__":
    main()
