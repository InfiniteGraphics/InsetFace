"""Exercise the production curved-region solver without loading Metasequoia.

Run from a Visual Studio developer shell: python tests/curved_region_regression.py
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / "plugin/InsetFace.cpp").read_text(encoding="utf-8-sig")


def function(signature):
    start = source.index(signature)
    opening = source.index("{", start)
    level = 1
    end = opening + 1
    while level:
        level += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


stub = r'''
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <cassert>
#include <iostream>
#include <map>
#include <limits>
using UINT=unsigned;
struct MQCoordinate {float u,v;};
struct MQPoint {
 float x,y,z;
 MQPoint(float a=0,float b=0,float c=0):x(a),y(b),z(c){}
 MQPoint operator+(const MQPoint& p)const{return {x+p.x,y+p.y,z+p.z};}
 MQPoint operator-(const MQPoint& p)const{return {x-p.x,y-p.y,z-p.z};}
 MQPoint operator*(float s)const{return {x*s,y*s,z*s};}
 MQPoint operator/(float s)const{return *this*(1/s);}
 MQPoint& operator+=(const MQPoint& p){*this=*this+p;return *this;}
};
float GetInnerProduct(const MQPoint& a,const MQPoint& b){return a.x*b.x+a.y*b.y+a.z*b.z;}
MQPoint GetCrossProduct(const MQPoint& a,const MQPoint& b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
MQPoint Normalize(const MQPoint& p){return p/std::sqrt(GetInnerProduct(p,p));}
struct Point2D{double X,Y;};
static const double kGeometryEpsilon=1e-6,kAngleEpsilon=1e-8,kMaxMiterFactor=8;
static const int kInteriorSolveIterations=80;
'''
types = source[source.index("struct TempVertex"):source.index("struct RegionCommitData")]
declaration = r'''
class InsetFacePlugin {
public:
 struct {double Thickness=0;bool EvenOffset=false;} m_Params;
 bool BuildSurfaceAwareBoundaryTargets(TempRegion&);
 bool SolveRegionInterior3D(TempRegion&,double);
 void AppendLocalizedWarning(std::wstring& w,const char* key)const {while(*key)w+=*key++;}
};
'''
main = r'''
// A front panel, four narrow round-over panels, and a top panel.
// No LocalInsetPoints are populated: region insets must not require them.
TempRegion strip(int columns=2) {
 TempRegion r;
 const float ys[]={0,4,4.382683f,4.707107f,4.923880f,5,5};
 const float zs[]={0,0,.076120f,.292893f,.617317f,1,5};
 for(int row=0;row<7;++row)for(int col=0;col<columns;++col){
  TempVertex v;v.OriginalPosition={10.f*col/(columns-1),ys[row],zs[row]};
  v.NewPosition={99,99,99};r.Vertices.push_back(v);
 }
 std::map<std::pair<int,int>,int> edges;
 for(int row=0;row<6;++row)for(int col=0;col<columns-1;++col){
  TempFace f;f.Vertices={row*columns+col,row*columns+col+1,(row+1)*columns+col+1,(row+1)*columns+col};
  auto a=r.Vertices[f.Vertices[0]].OriginalPosition,b=r.Vertices[f.Vertices[1]].OriginalPosition,c=r.Vertices[f.Vertices[2]].OriginalPosition;
  f.FaceNormal=Normalize(GetCrossProduct(b-a,c-a));
  for(int i=0;i<4;++i){
   TempHalfEdge e;e.StartVertex=f.Vertices[i];e.EndVertex=f.Vertices[(i+1)%4];e.FaceIndex=(int)r.Faces.size();
   e.Next=(int)r.HalfEdges.size()+1;e.Prev=(int)r.HalfEdges.size()-1;
   int index=(int)r.HalfEdges.size();edges[{e.StartVertex,e.EndVertex}]=index;
   f.HalfEdges.push_back(index);r.HalfEdges.push_back(e);
   r.Vertices[e.StartVertex].AverageNormal+=f.FaceNormal;
  }
  r.Faces.push_back(f);
 }
 for(auto& e:r.HalfEdges){auto found=edges.find({e.EndVertex,e.StartVertex});
  e.Boundary=found==edges.end();if(!e.Boundary)e.Pair=found->second;
  if(e.Boundary){r.Vertices[e.StartVertex].Boundary=true;r.Vertices[e.EndVertex].Boundary=true;}}
 for(auto& v:r.Vertices)v.AverageNormal=Normalize(v.AverageNormal);
 return r;
}
bool near(float a,double b){return std::fabs(a-b)<2e-5;}
int main(){
 InsetFacePlugin p;
 for(bool even:{false,true})for(double thickness:{0.,.1,1.,-1.}){
  auto r=strip();p.m_Params.Thickness=thickness;p.m_Params.EvenOffset=even;
  assert(p.BuildSurfaceAwareBoundaryTargets(r));assert(p.SolveRegionInterior3D(r,0));
  for(int row=1;row<6;++row)for(int col=0;col<2;++col){
   const auto& v=r.Vertices[row*2+col];
   assert(near(v.NewPosition.x,col?10-thickness:thickness));
   assert(near(v.NewPosition.y,v.OriginalPosition.y));assert(near(v.NewPosition.z,v.OriginalPosition.z));
  }
  if(thickness==0)for(const auto& v:r.Vertices){assert(near(v.NewPosition.x,v.OriginalPosition.x));assert(near(v.NewPosition.y,v.OriginalPosition.y));assert(near(v.NewPosition.z,v.OriginalPosition.z));}
 }
 // Interior displacement must be initialized independently of stale positions.
 for(double depth:{0.,.3}){
  auto r=strip(5);p.m_Params.Thickness=0;
  assert(p.BuildSurfaceAwareBoundaryTargets(r));assert(p.SolveRegionInterior3D(r,depth));
  for(const auto& v:r.Vertices){auto expected=v.OriginalPosition+v.AverageNormal*(float)depth;
   assert(near(v.NewPosition.x,expected.x));assert(near(v.NewPosition.y,expected.y));assert(near(v.NewPosition.z,expected.z));}
 }
 std::cout<<"PASS: curved strip boundary spacing, narrow facets, both offset modes, zero/negative thickness, and interior zero/depth preservation\n";
}
'''
program = stub + types + declaration
for signature in ("static MQPoint ProjectVectorToPlane(", "bool InsetFacePlugin::BuildSurfaceAwareBoundaryTargets(", "bool InsetFacePlugin::SolveRegionInterior3D("):
    program += function(signature)
out = root / "build/review"
out.mkdir(parents=True, exist_ok=True)
cpp = out / "curved_region_regression.cpp"
cpp.write_text(program + main, encoding="utf-8")
subprocess.run(["cl", "/nologo", "/EHsc", "/std:c++14", "/W4", str(cpp),
                "/Fe:" + str(out / "curved_region_regression.exe"), "/Fo:" + str(out / "curved_region_regression.obj")], check=True)
subprocess.run([str(out / "curved_region_regression.exe")], check=True)
