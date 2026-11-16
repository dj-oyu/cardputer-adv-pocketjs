"""Offline affine rig-space reference, not a skinning or pose evaluator.

Column vectors, row-major 4x4 matrices in original source coordinates.
P and B are evaluated pose/rest globals in armature space, not local basis.
No axis conversion, TRS decomposition, constraint evaluation or LBS substitution.
"""
import math


def affine(m):
    if len(m) != 4 or any(len(row) != 4 for row in m):
        raise ValueError('expected 4x4')
    if any(type(x) not in (int, float) or not math.isfinite(x) for row in m for x in row):
        raise ValueError('nonfinite matrix')
    if m[3] != [0, 0, 0, 1]:
        raise ValueError('not affine')
    return [[float(x) for x in row] for row in m]


def mul(a, b):
    a=affine(a); b=affine(b)
    return affine([[sum(a[r][k]*b[k][c] for k in range(4)) for c in range(4)] for r in range(4)])


def inverse(m):
    a=affine(m)
    work=[a[r][:]+[float(r==c) for c in range(4)] for r in range(4)]
    for c in range(4):
        pivot=max(range(c,4),key=lambda r:abs(work[r][c]))
        if work[pivot][c]==0: raise ValueError('singular matrix')
        work[c],work[pivot]=work[pivot],work[c]
        scale=work[c][c]; work[c]=[v/scale for v in work[c]]
        for r in range(4):
            if r!=c:
                factor=work[r][c]
                work[r]=[v-factor*w for v,w in zip(work[r],work[c])]
    return affine([row[4:] for row in work])


def mesh_palette(mesh_world, armature_world, rest_global, pose_global):
    """Node-local matrix C: M^-1 A P B^-1 A^-1 M. Apply node M once later.

    This is the affine bone transform only. In particular it is NOT permission
    to linear-blend matrices for a preserve-volume/dual-quaternion modifier.
    Caller reevaluates P/A/M for each pose; rest B is immutable source bind data.
    """
    result=inverse(mesh_world)
    for matrix in [armature_world,pose_global,inverse(rest_global),inverse(armature_world),mesh_world]:
        result=mul(result,matrix)
    return result


def dq_factors(mesh_world, armature_world, rest_global, pose_global):
    """Keep DQ blend in armature space as in the existing rigid DQ oracle.

    Transform input by to_rig, blend with validated rigid palette, then transform
    result by from_rig. Do not convert the mesh-conjugated affine palette to a
    quaternion: nonuniform mesh scale/shear generally makes that non-rigid.
    """
    to_rig=mul(inverse(armature_world),mesh_world)
    from_rig=inverse(to_rig)
    bone=mul(pose_global,inverse(rest_global))
    return to_rig,bone,from_rig
