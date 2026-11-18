"""Offline mesh-attached fill for an art-directed face lighting palette."""

from fnmatch import fnmatchcase
from math import hypot


def _smoothstep(value):
    value = min(1.0, max(0.0, value))
    return value * value * (3.0 - 2.0 * value)


def _weight(point, region):
    x, y, z = point
    cx, cz = region["center_xz"]
    rx, rz = region["radii_xz"]
    if region.get("mirror_x"):
        x = abs(x)
    edge = hypot((x - cx) / rx, (z - cz) / rz)
    core = region["core"]
    falloff = _smoothstep((1.0 - edge) / (1.0 - core))
    inner, outer = region["front_y"]
    front = _smoothstep((outer - y) / (outer - inner))
    return falloff * front * region["strength"]


def polygon_fill(mesh_name, center, vertices, regions):
    """Use authored neutral coordinates and carry the result by polygon ID."""
    samples = (center, *vertices)
    weight = 0.0
    for region in regions:
        if fnmatchcase(mesh_name, region["mesh"]):
            # A half-covered edge polygon gets softer fill than one in the core.
            coverage = sum(_weight(point, region) for point in samples) / len(samples)
            weight = max(weight, coverage)
    return weight


def apply_face_fill(shaded, albedo, key_cosine, weight):
    """Blend a warm frontal fill in linear RGB, preserving a slight key gradient."""
    if weight == 0:
        return shaded
    exposure = 0.73 + 0.16 * key_cosine
    warm = (1.08, 0.91, 0.82)
    return tuple(
        color * (1.0 - weight) + reflectance * tint * exposure * weight
        for color, reflectance, tint in zip(shaded, albedo, warm, strict=True)
    )
