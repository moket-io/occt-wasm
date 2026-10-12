// Entry points exported to JavaScript. Every function returns a plain JS
// object; none of them lets a C++ exception escape.
//
// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include <emscripten/val.h>

namespace occtwasm {

using emscripten::val;

// Module info and memory.
val version();
val heapStats();
val release(val id);
val releaseAll();

// Construction.
val makeBox(val dx, val dy, val dz);
val makeCylinder(val axisOrigin, val axisDir, val radius, val height);
val makeProfileFace(val plane, val loops);

// Operations with history.
val extrude(val shape, val dir, val distance);
val revolve(val shape, val axisOrigin, val axisDir, val angle);
val booleanOp(val op, val a, val b);
val fillet(val shape, val edges, val radius);
val chamfer(val shape, val edges, val distance);
val transform(val shape, val matrix);

// Queries.
val topology(val shape);
val properties(val shape);
val mesh(val shape, val linearDeflection, val angularDeflection);
val check(val shape);
val distance(val a, val subA, val b, val subB);

// Drawings: hidden line removal.
val project(val shape, val view);

// Exchange.
val exportStep(val shapes);
val importStep(val bytes);
val exportBrep(val shape);
val importBrep(val bytes);

} // namespace occtwasm
