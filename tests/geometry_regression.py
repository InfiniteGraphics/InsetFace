"""Generate a standalone regression executable from the current production functions.

Run in a Visual Studio developer shell: python tests/geometry_regression.py
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / "plugin/InsetFace.cpp").read_text(encoding="utf-8-sig")
out = root / "build/review"
out.mkdir(parents=True, exist_ok=True)
helpers = source[source.index("struct Point2D"):source.index("static double PolygonArea3D")]
methods = source[source.index("bool InsetFacePlugin::OffsetLoop2D("):source.index("bool InsetFacePlugin::BuildSurfaceAwareBoundaryTargets(")]
stub = r'''
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <limits>
#include <cassert>
#include <iostream>
static const double kGeometryEpsilon=1e-6, kAngleEpsilon=1e-8, kMaxMiterFactor=8;
static const int kInteriorSolveIterations=80;
'''
types = r'''
struct MQPoint {
 double x,y,z;
 MQPoint(double a=0,double b=0,double c=0):x(a),y(b),z(c){}
 MQPoint operator+(const MQPoint& p)const{return {x+p.x,y+p.y,z+p.z};}
 MQPoint operator*(double s)const{return {x*s,y*s,z*s};}
};
struct TempVertex { Point2D OriginalProjected,NewProjected; MQPoint OriginalPosition,NewPosition,AverageNormal; };
struct TempHalfEdge { int StartVertex,EndVertex; };
struct TempLoop { std::vector<int> Vertices; bool IsHole=false; };
struct TempRegion { std::vector<TempVertex> Vertices; std::vector<TempHalfEdge> HalfEdges; std::vector<TempLoop> BoundaryLoops; std::wstring Warning; MQPoint PlaneOrigin,PlaneU={1,0,0},PlaneV={0,1,0}; };
void AppendWarning(std::wstring& a,const std::wstring& b){a+=b;}
class InsetFacePlugin {
public:
 bool SolveRegionInterior2D(TempRegion&,double,double,bool);
 MQPoint LiftPoint(const TempRegion&,const Point2D&) const;
 void AppendLocalizedWarning(std::wstring& w,const char* key) const {while(*key)w+=*key++;w+=L";";}
 bool OffsetLoop2D(const std::vector<Point2D>&,double,bool,std::vector<Point2D>&,std::wstring&,bool=true) const;
};
'''
main = r'''
using Loop=std::vector<Point2D>;
TempRegion regionFrom(const std::vector<Loop>& loops) {
 TempRegion r;
 for(size_t i=0;i<loops.size();++i){
  TempLoop loop;loop.IsHole=i>0;
  for(auto q:loops[i]){loop.Vertices.push_back((int)r.Vertices.size());TempVertex v;
   v.OriginalProjected=v.NewProjected=q;v.OriginalPosition={q.X,q.Y,0};r.Vertices.push_back(v);}
  r.BoundaryLoops.push_back(loop);
 }
 return r;
}
bool clamped(const std::wstring& w){return w.find(L"WarnCollapsedClamped")!=std::wstring::npos;}
int main(){
 InsetFacePlugin p; Loop result;
 Loop square={{0,0},{2,0},{2,2},{0,2}};
 for(double t:{1.0,1.1,3.0,1000.0}){
  std::wstring w;assert(p.OffsetLoop2D(square,t,true,result,w));assert(clamped(w));
  assert(result[0].X>0 && result[0].X<1);assert(result[1].X>result[0].X);
 }
 for(double t:{0.0,0.25,-0.5}){
  std::wstring w;assert(p.OffsetLoop2D(square,t,true,result,w));assert(!clamped(w));
  assert(std::fabs(result[0].X-t)<1e-8);
 }
 std::reverse(square.begin(),square.end());
 {std::wstring w;assert(p.OffsetLoop2D(square,3,true,result,w));assert(clamped(w));}
 Loop concave={{0,0},{4,0},{4,1},{1,1},{1,4},{0,4}};
 for(double t:{0.1,-0.1}){std::wstring w;assert(p.OffsetLoop2D(concave,t,true,result,w));assert(!clamped(w));}
 Loop collinear={{0,0},{1,0},{2,0},{2,2},{0,2}};
 {std::wstring w;assert(p.OffsetLoop2D(collinear,0.1,true,result,w));assert(!clamped(w));}
 Loop outer={{0,0},{10,0},{10,10},{0,10}}, hole={{4,4},{4,6},{6,6},{6,4}};
 for(double t:{2.0,2.1,20.0}){
  auto r=regionFrom({outer,hole});assert(p.SolveRegionInterior2D(r,t,0,true));assert(clamped(r.Warning));
  double d=r.Vertices[0].NewProjected.X;assert(d>0 && d<2);
  assert(std::fabs(r.Vertices[4].NewProjected.X-(4-d))<1e-8);
 }
 {auto r=regionFrom({outer,hole});assert(p.SolveRegionInterior2D(r,-3,0,true));assert(clamped(r.Warning));
  double d=r.Vertices[0].NewProjected.X;assert(d<0 && d>-1);
  assert(std::fabs(r.Vertices[4].NewProjected.X-(4-d))<1e-8);}
 Loop h1={{2,4},{2,6},{4,6},{4,4}},h2={{5,4},{5,6},{7,6},{7,4}};
 {auto r=regionFrom({outer,h1,h2});assert(p.SolveRegionInterior2D(r,0.75,0,true));assert(clamped(r.Warning));
  double d=r.Vertices[0].NewProjected.X;assert(d>0 && d<0.5);
  assert(std::fabs(r.Vertices[4].NewProjected.X-(2-d))<1e-8);
  assert(std::fabs(r.Vertices[8].NewProjected.X-(5-d))<1e-8);}
 // Contours exchange places with valid endpoints: only continuous collision catches it.
 Loop a={{0,0},{1,0},{1,1},{0,1}},b={{3,0},{4,0},{4,1},{3,1}};
 assert(!BoundaryMotionIsValid2D({a,b},{b,a}));
 std::cout<<"Geometry regression checks passed\n";
}
'''
cpp = out / "geometry_regression.cpp"
cpp.write_text(stub + helpers + types + methods + main, encoding="utf-8")
subprocess.run(["cl", "/nologo", "/EHsc", "/std:c++14", "/W4", str(cpp),
                "/Fe:" + str(out / "geometry_regression.exe"), "/Fo:" + str(out / "geometry_regression.obj")], check=True)
subprocess.run([str(out / "geometry_regression.exe")], check=True)
