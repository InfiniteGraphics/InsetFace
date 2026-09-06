"""Regression checks for the whole-surface Region execution path.

The test intentionally reads the production source.  It catches accidental
reintroduction of the old per-face prerequisite, while the small topology
model checks that the three round-over strips are interior to one Region.
Run from the repository root with ``python tests/region_surface_structure_regression.py``.
"""
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "plugin/InsetFace.cpp").read_text(encoding="utf-8-sig")


def body_after(signature: str) -> str:
    start = SOURCE.index(signature)
    opening = SOURCE.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]


def compact(text: str) -> str:
    return re.sub(r"\s+", "", text)


def strip_topology(columns=2, strips=5):
    """Return boundary and interior edges for top + round-over + front strips."""
    faces = []
    for row in range(strips):
        for col in range(columns - 1):
            a = row * columns + col
            faces.append((a, a + 1, a + columns + 1, a + columns))
    counts = {}
    for face in faces:
        for i, a in enumerate(face):
            edge = tuple(sorted((a, face[(i + 1) % len(face)])))
            counts[edge] = counts.get(edge, 0) + 1
    boundary = {edge for edge, count in counts.items() if count == 1}
    interior = {edge for edge, count in counts.items() if count == 2}
    assert all(count in (1, 2) for count in counts.values())
    assert len(interior) == strips - 1  # the horizontal joins are internal
    assert len(boundary) == 2 * strips + 2 * (columns - 1)
    return boundary, interior


def main():
    build = body_after("bool InsetFacePlugin::BuildTempRegion(")
    build_c = compact(build)
    assert "constboolregion_mode=m_Params.CurrentMode==InsetParameters::ModeRegion;" in build_c
    assert "SolveRegionEvenOffsetPlanar" not in build
    assert "SolveRegionEvenOffsetSurfaceAware" not in build
    assert build_c.index("if(region_mode)") < build_c.index("BuildFaceLocalInset(")
    assert "SolveRegionPlanar(region,m_Params.Thickness,m_Params.Depth,m_Params.EvenOffset)" in build_c
    assert "SolveRegionSurfaceAware(region,m_Params.Thickness,m_Params.Depth,m_Params.EvenOffset)" in build_c
    individual = build[build.index("else") :]
    assert "BuildFaceLocalInset" in individual and "SolvePatchInsetVertices" in individual

    planar = body_after("bool InsetFacePlugin::SolveRegionInterior2D(")
    assert "bool even_offset" in planar.split("{", 1)[0]
    assert "OffsetLoop2D" in planar and "even_offset" in planar

    boundary = body_after("bool InsetFacePlugin::BuildSurfaceAwareBoundaryTargets(")
    assert "bool even_offset" in boundary.split("{", 1)[0]
    assert "even_offset" in boundary
    assert "ProjectVectorToPlane" in boundary
    assert "kMaxMiterFactor" in boundary

    interior = body_after("bool InsetFacePlugin::SolveRegionInterior3D(")
    assert "edge_length" in interior or "EdgeLength" in interior or "length" in interior.lower()
    assert "ProjectVectorToPlane" in interior or "DotProduct" in interior or "GetInnerProduct" in interior

    boundary_edges, interior_edges = strip_topology()
    assert boundary_edges.isdisjoint(interior_edges)
    print("PASS: Region routes both offset modes through surface-aware solvers; "
          "round-over joins remain interior edges; Individual keeps local inset path")


if __name__ == "__main__":
    main()
