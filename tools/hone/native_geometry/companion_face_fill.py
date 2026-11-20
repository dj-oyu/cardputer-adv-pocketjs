"""Offline mesh-attached fill for an art-directed face lighting palette."""

from colorsys import hls_to_rgb, rgb_to_hls
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


def _linear(channel):
    return channel / 12.92 if channel <= 0.04045 else ((channel + 0.055) / 1.055) ** 2.4


def _srgb(channel):
    return 12.92 * channel if channel <= 0.0031308 else 1.055 * channel ** (1 / 2.4) - 0.055


def _luminance(rgb):
    return sum(weight * _linear(value) for weight, value in zip((0.2126, 0.7152, 0.0722), rgb, strict=True))


def harmonize_face_color(rgb, fill_weight):
    """Use the selected warm-coat grade without lowering the eye fill."""
    if fill_weight <= 0.0:
        return rgb
    hue, lightness, saturation = rgb_to_hls(*rgb)
    if 45 / 360 < hue < 344 / 360 or saturation <= 0.25 or not 0.19 < lightness < 0.71:
        return rgb
    strength = _smoothstep(fill_weight / 0.28)
    hue = (hue - strength * 8 / 360) % 1
    saturation *= 1 - 0.30 * strength
    target = _luminance(rgb)
    low, high = 0.0, 1.0
    for _ in range(20):
        mid = (low + high) / 2
        if _luminance(hls_to_rgb(hue, mid, saturation)) < target:
            low = mid
        else:
            high = mid
    return hls_to_rgb(hue, (low + high) / 2, saturation)


def apply_face_highlight(rgb, color, weight):
    """Mix the authored highlight in linear light before RGB565 quantization."""
    if weight <= 0.0:
        return rgb
    return tuple(
        _srgb(_linear(base) * (1.0 - weight) + _linear(highlight) * weight)
        for base, highlight in zip(rgb, color, strict=True)
    )
