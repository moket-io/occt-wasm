import type {
  Failure,
  LineSegment,
  Loop,
  OcctKernel,
  Plane,
  ProfileFaceResult,
  ShapeId,
  SweepResult,
  OpResult,
} from '../dist/index.js';

type Ok<T> = Exclude<T, Failure>;

export declare const XY: Plane;
export declare function ok<T extends { ok: boolean }>(result: T): Exclude<T, { ok: false }>;
export declare function rectLoop(x0: number, y0: number, w: number, h: number): LineSegment[];
export declare function circleLoop(cx: number, cy: number, r: number): Loop;

export interface R1 {
  profile: Ok<ProfileFaceResult>;
  extrude: Ok<SweepResult>;
  shape: ShapeId;
}
export declare function buildR1(k: OcctKernel): R1;
export declare function buildR2(k: OcctKernel): R1;
export declare function edgesAtZ(k: OcctKernel, shape: ShapeId, z: number, tol?: number): number[];
export declare function buildR3(k: OcctKernel): {
  r1: R1;
  topEdges: number[];
  fillet: Ok<OpResult>;
  shape: ShapeId;
};

export declare const R1_VOLUME: number;
export declare const R2_VOLUME: number;
export declare const R3_VOLUME: number;
