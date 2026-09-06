"""Extract the current planar solver and regression-test it with cl or C++.

Windows: run from a VS Developer Command Prompt:
    python tests/run_region_solver_tests.py
"""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "plugin/InsetFace.cpp").read_text(encoding="utf-8-sig")
solver = source.split("bool InsetFacePlugin::SolveRegionInterior2D(", 1)[1].split(
    "bool InsetFacePlugin::BuildSurfaceAwareBoundaryTargets(", 1)[0]
body = solver[solver.index("\tfor (int iteration = 0;"):solver.rindex("\treturn true;")]
point = source[source.index("struct Point2D"):source.index("static double Dot2D")]

# Guard the path dependency: small valid internal faces must not require a local inset.
region = source.split("bool InsetFacePlugin::BuildTempRegion(", 1)[1].split(
    "void InsetFacePlugin::", 1)[0]
compact = re.sub(r"\s+", "", region)
assert compact.index("nearly_planar=IsRegionNearlyPlanar(") < compact.index("BuildFaceLocalInset(")
assert "for(size_tfi=0;!region_mode&&fi<region.Faces.size();++fi){if(!BuildFaceLocalInset(" in compact
assert "if(nearly_planar){solved=SolveRegionPlanar(" in compact
assert "!std::isfinite(area)||area<=0.0" in compact
print("PASS: planar path bypasses individual inset; positive tiny input area is accepted")

PRE = r"""struct MQPoint { double x,y,z; MQPoint(double a=0,double b=0,double c=0):x(a),y(b),z(c){} MQPoint operator+(const MQPoint& p) const {return MQPoint(x+p.x,y+p.y,z+p.z);} MQPoint operator*(float v) const {return MQPoint(x*v,y*v,z*v);} };
struct TempVertex { Point2D OriginalProjected, NewProjected; MQPoint OriginalPosition, NewPosition, AverageNormal; };
struct Region { std::vector<TempVertex> Vertices; MQPoint PlaneU{1,0,0},PlaneV{0,1,0}; };
static const int kInteriorSolveIterations=80;
void solve(Region& region, const std::vector<bool>& fixed, const std::vector<std::vector<int>>& adjacency, double depth) {
"""
POST = r"""}
int main() {
 for (int test=0; test<3; ++test) {
  Region r; r.Vertices.resize(5);
  const double xs[]={0,2,2,0,.3}, ys[]={0,0,2,2,.4};
  for(int i=0;i<5;++i) {
   auto& v=r.Vertices[i]; v.OriginalProjected=Point2D(xs[i],ys[i]);v.NewProjected=v.OriginalProjected;
   v.OriginalPosition=MQPoint(xs[i],ys[i],i*.003);v.AverageNormal=MQPoint(0,0,1);
   if(i<4 && test==1)v.NewProjected=v.NewProjected+Point2D(.12,-.23);
  }
  solve(r,{true,true,true,true,false},{{4},{4},{4},{4},{0,1,2,3}},test==2?.7:0);
  for(int i=0;i<5;++i) {
   const auto& p=r.Vertices[i].NewPosition;
   assert(std::fabs(p.x-(xs[i]+(test==1?.12:0)))<1e-6);
   assert(std::fabs(p.y-(ys[i]+(test==1?-.23:0)))<1e-6);
   assert(std::fabs(p.z-(i*.003+(test==2?.7:0)))<1e-6);
  }
 }
 std::cout<<"PASS: zero displacement preserves off-center vertex; uniform displacement propagates; depth retains original height\n";
}
"""

program = "#include <vector>\n#include <cassert>\n#include <cmath>\n#include <iostream>\n" + point + PRE + body + POST
compiler = os.environ.get("CXX") or shutil.which("cl") or shutil.which("c++") or shutil.which("g++")
if not compiler:
    raise SystemExit("No C++ compiler found. Run from a VS Developer Command Prompt or set CXX.")
with tempfile.TemporaryDirectory(prefix="inset-region-test-") as folder:
    folder = Path(folder)
    cpp = folder / "region_solver.cpp"
    exe = folder / ("region_solver.exe" if os.name == "nt" else "region_solver")
    cpp.write_text(program, encoding="utf-8")
    if Path(compiler).stem.lower() == "cl":
        command = [compiler, "/nologo", "/EHsc", "/std:c++14", str(cpp), "/Fe" + str(exe)]
    else:
        command = [compiler, "-std=c++14", str(cpp), "-o", str(exe)]
    subprocess.run(command, cwd=folder, check=True)
    subprocess.run([str(exe)], cwd=folder, check=True)
