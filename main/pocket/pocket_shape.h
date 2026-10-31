#ifndef POCKET_SHAPE_H
#define POCKET_SHAPE_H
#include "quickjs.h"
#include "esp_err.h"

/* pocket.shapes: flat-shaded vector drawing for the procedural surface,
 * written in C because the same geometry in JS cost MEGADEMO's WAKE ~130 ms a
 * frame on the device (docs/apps/megademo-shapes.md). project() turns model
 * points into screen points; fill() scan-converts a polygon or an ellipse
 * less polygon holes into horizontal-line trapezoid draws; outline() draws a
 * polygon's edges, leaving out those inside other polygons. These write
 * directly into the candidate procedural frame under its existing limits.
 * gouraudPatch() snapshots bounded screen vertices/indices/colour fields
 * and holes, interpolating RGB565 without a framebuffer or depth sort.
 * shade() is a separate bounded SNORM16 normal/albedo/thickness kernel:
 * borrowed typed arrays in, RGB565 sample colours out, no frame required.
 * Not part of the common API: app_session.c installs it only into a session
 * whose manifest names kasane.shapes, as it does pocket.derby. */
esp_err_t pocket_shape_install(JSContext *ctx, void *unused);
/* The namespace's functions on ns; what install's lazy builder runs. The host
 * harnesses call it directly. */
esp_err_t pocket_shape_fill(JSContext *ctx, JSValueConst ns, void *unused);
#endif
