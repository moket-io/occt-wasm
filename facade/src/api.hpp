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
val makeVertex(val point);
val subShape(val shape, val kind, val index);
val makeProfileFace(val plane, val loops);
val makeWire(val plane, val segments);
val joinEdges(val parts);

// Operations with history.
val extrude(val shape, val dir, val distance);
val revolve(val shape, val axisOrigin, val axisDir, val angle);
val booleanOp(val op, val a, val b);
val fillet(val shape, val edges, val radius);
val chamfer(val shape, val edges, val distance);
val transform(val shape, val matrix);
val thickSolid(val shape, val faces, val offset, val options);
val offsetShape(val shape, val offset, val options);
val draft(val shape, val faces, val direction, val angle, val neutral);
val sweep(val profile, val spine, val options);
val helix(val axisOrigin, val axisDir, val radius, val pitch, val height, val options);
val loft(val sections, val options);

// Queries.
val topology(val shape);
val properties(val shape);
val mesh(val shape, val linearDeflection, val angularDeflection);
val check(val shape);
val distance(val a, val subA, val b, val subB);

// Drawings: hidden line removal.
val project(val shapes, val view);

// Exchange.
val exportStep(val shapes, val options);
// exportStep with options: one product, its name and colours (assembly.cpp).
val exportStepColored(val shapes, val options);
val importStep(val bytes);
val importStepParts(val bytes, val options);
val exportBrep(val shape);
val importBrep(val bytes);

// Assemblies: XCAF products and named instances in one STEP file.
val exportStepAssembly(val assembly);

} // namespace occtwasm
