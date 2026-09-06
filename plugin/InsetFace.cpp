//----------------------------------------------------------------------------
//
//   InsetFace
//
//----------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "MQBasePlugin.h"
#include "MQ3DLib.h"
#include "MQSetting.h"
#include "MQSelectOperation.h"
#include "MQWidget.h"
#include "Language.h"

static const DWORD kInsetFaceProductId = 0x56A31D20;
static const DWORD kInsetFacePluginId = 0x5A1E7F44;
static const wchar_t* kResourceFileName = L"InsetFace.resource.xml";

static const double kThicknessScale = 0.05;
static const int kDragThresholdPixels = 4;
static const double kGeometryEpsilon = 1e-6;
static const double kAngleEpsilon = 1e-8;
static const int kInteriorSolveIterations = 80;
static const double kMaxMiterFactor = 8.0;

MQBasePlugin* GetPluginClass();

static std::wstring Trim(const std::wstring& value)
{
	size_t first = 0;
	while (first < value.size() && iswspace(value[first])) {
		++first;
	}
	size_t last = value.size();
	while (last > first && iswspace(value[last - 1])) {
		--last;
	}
	return value.substr(first, last - first);
}

static std::wstring ToLowerWide(std::wstring value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
		return static_cast<wchar_t>(towlower(ch));
	});
	return value;
}

static std::wstring GetDirectoryName(const std::wstring& path)
{
	size_t pos = path.find_last_of(L"\\/");
	if (pos == std::wstring::npos) return std::wstring();
	return path.substr(0, pos);
}

static std::wstring JoinPath(const std::wstring& left, const std::wstring& right)
{
	if (left.empty()) return right;
	if (right.empty()) return left;
	const wchar_t tail = left[left.size() - 1];
	if (tail == L'\\' || tail == L'/') {
		return left + right;
	}
	return left + L"\\" + right;
}

static std::wstring GetPluginModuleDirectory()
{
	HMODULE module = NULL;
	if (!GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&kInsetFaceProductId),
		&module)) {
		return std::wstring();
	}

	wchar_t path[MAX_PATH] = {};
	DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
	if (length == 0 || length >= MAX_PATH) {
		return std::wstring();
	}

	return GetDirectoryName(std::wstring(path, length));
}

static std::wstring NormalizeLanguageName(std::wstring language)
{
	language = ToLowerWide(Trim(language));
	if (language.empty()) return L"English";

	if (language == L"ja" || language == L"ja-jp" || language.find(L"japan") != std::wstring::npos) {
		return L"Japanese";
	}
	if (language == L"en" || language == L"en-us" || language == L"en-gb" || language.find(L"english") != std::wstring::npos) {
		return L"English";
	}
	if (language == L"zh" || language == L"zh-cn" || language == L"zh-sg" || language.find(L"chinese") != std::wstring::npos || language.find(L"zh-hans") != std::wstring::npos) {
		return L"Chinese";
	}

	return language;
}

struct Point2D
{
	double X;
	double Y;

	Point2D() : X(0.0), Y(0.0) {}
	Point2D(double x, double y) : X(x), Y(y) {}

	Point2D operator+(const Point2D& rhs) const { return Point2D(X + rhs.X, Y + rhs.Y); }
	Point2D operator-(const Point2D& rhs) const { return Point2D(X - rhs.X, Y - rhs.Y); }
	Point2D operator*(double s) const { return Point2D(X * s, Y * s); }
	Point2D operator/(double s) const { return Point2D(X / s, Y / s); }
	Point2D& operator+=(const Point2D& rhs) { X += rhs.X; Y += rhs.Y; return *this; }
};

static double Dot2D(const Point2D& a, const Point2D& b)
{
	return a.X * b.X + a.Y * b.Y;
}

static double Cross2D(const Point2D& a, const Point2D& b)
{
	return a.X * b.Y - a.Y * b.X;
}

static double Length2D(const Point2D& v)
{
	return std::sqrt(Dot2D(v, v));
}

static Point2D Normalize2D(const Point2D& v)
{
	double len = Length2D(v);
	if (len <= kGeometryEpsilon) return Point2D(0.0, 0.0);
	return v / len;
}

static Point2D PerpLeft(const Point2D& v)
{
	return Point2D(-v.Y, v.X);
}

static double PolygonArea2D(const std::vector<Point2D>& polygon)
{
	if (polygon.size() < 3) return 0.0;
	double area = 0.0;
	for (size_t i = 0; i < polygon.size(); ++i) {
		const Point2D& a = polygon[i];
		const Point2D& b = polygon[(i + 1) % polygon.size()];
		area += Cross2D(a, b);
	}
	return area * 0.5;
}

static bool NearlyEqual(double a, double b, double epsilon)
{
	return std::fabs(a - b) <= epsilon;
}

static bool IntersectInfiniteLines(const Point2D& p0, const Point2D& d0, const Point2D& p1, const Point2D& d1, Point2D& out_point)
{
	double denom = Cross2D(d0, d1);
	if (std::fabs(denom) <= kAngleEpsilon) {
		return false;
	}
	double t = Cross2D(p1 - p0, d1) / denom;
	out_point = p0 + d0 * t;
	return true;
}

static double DistanceSquared2D(const Point2D& a, const Point2D& b)
{
	double dx = a.X - b.X;
	double dy = a.Y - b.Y;
	return dx * dx + dy * dy;
}

static int Orientation2D(const Point2D& a, const Point2D& b, const Point2D& c)
{
	double value = Cross2D(b - a, c - a);
	if (value > kGeometryEpsilon) return 1;
	if (value < -kGeometryEpsilon) return -1;
	return 0;
}

static bool OnSegment2D(const Point2D& a, const Point2D& b, const Point2D& p)
{
	return std::min(a.X, b.X) - kGeometryEpsilon <= p.X && p.X <= std::max(a.X, b.X) + kGeometryEpsilon &&
		std::min(a.Y, b.Y) - kGeometryEpsilon <= p.Y && p.Y <= std::max(a.Y, b.Y) + kGeometryEpsilon;
}

static bool SegmentsIntersect2D(const Point2D& a1, const Point2D& a2, const Point2D& b1, const Point2D& b2)
{
	int o1 = Orientation2D(a1, a2, b1);
	int o2 = Orientation2D(a1, a2, b2);
	int o3 = Orientation2D(b1, b2, a1);
	int o4 = Orientation2D(b1, b2, a2);

	if (o1 != o2 && o3 != o4) return true;
	if (o1 == 0 && OnSegment2D(a1, a2, b1)) return true;
	if (o2 == 0 && OnSegment2D(a1, a2, b2)) return true;
	if (o3 == 0 && OnSegment2D(b1, b2, a1)) return true;
	if (o4 == 0 && OnSegment2D(b1, b2, a2)) return true;
	return false;
}

static bool PolygonHasSelfIntersection(const std::vector<Point2D>& polygon)
{
	if (polygon.size() < 4) return false;
	for (size_t i = 0; i < polygon.size(); ++i) {
		Point2D a0 = polygon[i];
		Point2D a1 = polygon[(i + 1) % polygon.size()];
		for (size_t j = i + 1; j < polygon.size(); ++j) {
			size_t next_j = (j + 1) % polygon.size();
			if (i == j) continue;
			if ((i + 1) % polygon.size() == j) continue;
			if (i == next_j) continue;
			Point2D b0 = polygon[j];
			Point2D b1 = polygon[next_j];
			if (SegmentsIntersect2D(a0, a1, b0, b1)) {
				return true;
			}
		}
	}
	return false;
}

// Test the whole linear motion, so a contour cannot collapse and reopen
// between its original and final positions.
static bool MovingPointsMeet2D(const Point2D& a, const Point2D& da, const Point2D& b, const Point2D& db)
{
	Point2D separation = a - b;
	Point2D velocity = da - db;
	double speed_squared = Dot2D(velocity, velocity);
	double time = speed_squared > 0.0 ? std::max(0.0, std::min(1.0, -Dot2D(separation, velocity) / speed_squared)) : 0.0;
	return Length2D(separation + velocity * time) <= kGeometryEpsilon;
}

static bool MovingPointTouchesEdge2D(const Point2D& p, const Point2D& dp,
	const Point2D& a, const Point2D& da, const Point2D& b, const Point2D& db)
{
	Point2D edge = b - a, edge_velocity = db - da;
	Point2D relative = p - a, relative_velocity = dp - da;
	double c0 = Cross2D(edge, relative);
	double c1 = Cross2D(edge, relative_velocity) + Cross2D(edge_velocity, relative);
	double c2 = Cross2D(edge_velocity, relative_velocity);
	std::vector<double> times;
	times.push_back(0.0);
	times.push_back(1.0);
	double tolerance = 64.0 * std::numeric_limits<double>::epsilon() *
		std::max(1.0, std::max(std::fabs(c0), std::max(std::fabs(c1), std::fabs(c2))));
	if (std::fabs(c2) <= tolerance) {
		if (std::fabs(c1) > tolerance) times.push_back(-c0 / c1);
		else if (std::fabs(c0) <= tolerance) {
			// Permanently collinear segments first overlap at an endpoint.
			if (MovingPointsMeet2D(p, dp, a, da) || MovingPointsMeet2D(p, dp, b, db)) return true;
		}
	}
	else {
		double discriminant = c1 * c1 - 4.0 * c2 * c0;
		double discriminant_tolerance = 64.0 * std::numeric_limits<double>::epsilon() *
			std::max(1.0, c1 * c1 + std::fabs(4.0 * c2 * c0));
		if (discriminant >= -discriminant_tolerance) {
			double root = std::sqrt(std::max(0.0, discriminant));
			double q = -0.5 * (c1 + (c1 < 0.0 ? -root : root));
			if (q != 0.0) {
				times.push_back(q / c2);
				times.push_back(c0 / q);
			}
			else times.push_back(-c1 / (2.0 * c2));
		}
	}
	for (size_t i = 0; i < times.size(); ++i) {
		double t = times[i];
		if (t < 0.0 || t > 1.0) continue;
		Point2D point = p + dp * t, start = a + da * t, end = b + db * t;
		if (Orientation2D(start, end, point) == 0 && OnSegment2D(start, end, point)) return true;
	}
	return false;
}

static bool BoundaryMotionIsValid2D(const std::vector<std::vector<Point2D> >& source,
	const std::vector<std::vector<Point2D> >& target)
{
	for (size_t li = 0; li < source.size(); ++li) {
		for (size_t vi = 0; vi < source[li].size(); ++vi) {
			size_t next = (vi + 1) % source[li].size();
			const Point2D& p = source[li][vi];
			Point2D dp = target[li][vi] - p;
			if (!std::isfinite(target[li][vi].X) || !std::isfinite(target[li][vi].Y)) return false;
			if (MovingPointsMeet2D(p, dp, source[li][next], target[li][next] - source[li][next])) return false;
			for (size_t lj = 0; lj < source.size(); ++lj) {
				for (size_t ei = 0; ei < source[lj].size(); ++ei) {
					size_t end = (ei + 1) % source[lj].size();
					if (li == lj && (vi == ei || vi == end)) continue;
					if (MovingPointTouchesEdge2D(p, dp, source[lj][ei], target[lj][ei] - source[lj][ei],
						source[lj][end], target[lj][end] - source[lj][end])) return false;
				}
			}
		}
	}
	return true;
}

static double PolygonArea3D(const std::vector<MQPoint>& polygon)
{
	if (polygon.size() < 3) return 0.0;
	MQPoint origin = polygon[0];
	double area = 0.0;
	for (size_t i = 1; i + 1 < polygon.size(); ++i) {
		MQPoint cross = GetCrossProduct(polygon[i] - origin, polygon[i + 1] - origin);
		area += 0.5 * GetSize(cross);
	}
	return area;
}

static MQPoint ComputeFaceNormalFromPoints(const std::vector<MQPoint>& polygon)
{
	MQPoint normal(0, 0, 0);
	for (size_t i = 0; i < polygon.size(); ++i) {
		const MQPoint& a = polygon[i];
		const MQPoint& b = polygon[(i + 1) % polygon.size()];
		normal.x += (a.y - b.y) * (a.z + b.z);
		normal.y += (a.z - b.z) * (a.x + b.x);
		normal.z += (a.x - b.x) * (a.y + b.y);
	}
	return normal;
}

struct InsetParameters
{
	enum Mode
	{
		ModeRegion = 0,
		ModeIndividual = 1,
	};

	double Thickness;
	double Depth;
	bool EvenOffset;
	Mode CurrentMode;

	InsetParameters() : Thickness(0.0), Depth(0.0), EvenOffset(false), CurrentMode(ModeRegion) {}
};

struct SelectedFaceRef
{
	int ObjectIndex;
	UINT FaceUniqueID;

	SelectedFaceRef() : ObjectIndex(-1), FaceUniqueID(0) {}
	SelectedFaceRef(int object_index, UINT face_unique_id) : ObjectIndex(object_index), FaceUniqueID(face_unique_id) {}

	bool operator==(const SelectedFaceRef& rhs) const
	{
		return ObjectIndex == rhs.ObjectIndex && FaceUniqueID == rhs.FaceUniqueID;
	}

	bool operator<(const SelectedFaceRef& rhs) const
	{
		if (ObjectIndex != rhs.ObjectIndex) return ObjectIndex < rhs.ObjectIndex;
		return FaceUniqueID < rhs.FaceUniqueID;
	}
};

struct PreviewLine
{
	MQPoint A;
	MQPoint B;
};

struct PreviewObject
{
	std::vector<PreviewLine> Lines;
};

struct FaceInfo
{
	int FaceIndex;
	UINT FaceUniqueID;
	std::vector<int> Vertices;
	std::vector<MQCoordinate> UV;
	int MaterialIndex;
	MQPoint FaceNormal;
};

struct EdgeKey
{
	int A;
	int B;

	EdgeKey() : A(-1), B(-1) {}
	EdgeKey(int v0, int v1)
	{
		if (v0 < v1) {
			A = v0;
			B = v1;
		}
		else {
			A = v1;
			B = v0;
		}
	}

	bool operator<(const EdgeKey& rhs) const
	{
		if (A != rhs.A) return A < rhs.A;
		return B < rhs.B;
	}
};

struct TempVertex
{
	int OriginalVertexIndex;
	MQPoint OriginalPosition;
	MQPoint NewPosition;
	MQPoint AverageNormal;
	Point2D OriginalProjected;
	Point2D NewProjected;
	MQPoint SolvedTangentOffset;
	MQPoint BoundaryTargetPosition;
	bool Boundary;

	TempVertex()
		: OriginalVertexIndex(-1)
		, SolvedTangentOffset(0, 0, 0)
		, BoundaryTargetPosition(0, 0, 0)
		, Boundary(false)
	{
	}
};

struct TempHalfEdge
{
	int StartVertex;
	int EndVertex;
	int Next;
	int Prev;
	int Pair;
	int FaceIndex;
	int FaceVertexIndex;
	bool Boundary;

	TempHalfEdge()
		: StartVertex(-1)
		, EndVertex(-1)
		, Next(-1)
		, Prev(-1)
		, Pair(-1)
		, FaceIndex(-1)
		, FaceVertexIndex(-1)
		, Boundary(false)
	{
	}
};

struct TempFace
{
	int OriginalFaceIndex;
	UINT FaceUniqueID;
	int MaterialIndex;
	MQPoint FaceNormal;
	std::vector<int> Vertices;
	std::vector<MQCoordinate> UV;
	std::vector<int> HalfEdges;
	std::vector<MQPoint> LocalInsetPoints;
};

struct TempLoop
{
	std::vector<int> HalfEdges;
	std::vector<int> Vertices;
	bool IsHole;
	double Area;

	TempLoop() : IsHole(false), Area(0.0) {}
};

struct TempRegion
{
	int ObjectIndex;
	std::vector<TempVertex> Vertices;
	std::vector<TempHalfEdge> HalfEdges;
	std::vector<TempFace> Faces;
	std::vector<TempLoop> BoundaryLoops;
	MQPoint PlaneOrigin;
	MQPoint PlaneU;
	MQPoint PlaneV;
	MQPoint PlaneNormal;
	std::wstring Warning;
	bool Valid;

	TempRegion() : ObjectIndex(-1), Valid(false) {}
};

struct RegionCommitData
{
	int ObjectIndex;
	TempRegion Region;

	RegionCommitData() : ObjectIndex(-1) {}
};

struct PreviewMesh
{
	std::vector<PreviewObject> Objects;
	std::vector<RegionCommitData> CommitData;
	bool Valid;
	bool Complete;

	PreviewMesh() : Valid(false), Complete(false) {}

	void Clear()
	{
		Objects.clear();
		CommitData.clear();
		Valid = false;
		Complete = false;
	}
};

static void AppendWarning(std::wstring& target, const std::wstring& warning)
{
	if (warning.empty()) return;
	if (!target.empty()) target += L"; ";
	target += warning;
}

class InsetFacePlugin;

class InsetFaceWindow : public MQWindow
{
public:
	InsetFaceWindow(int id, InsetFacePlugin* plugin);
	~InsetFaceWindow();

	BOOL ThicknessChanged(MQWidgetBase* sender, MQDocument doc);
	BOOL DepthChanged(MQWidgetBase* sender, MQDocument doc);
	BOOL RegionModeChanged(MQWidgetBase* sender, MQDocument doc);
	BOOL IndividualModeChanged(MQWidgetBase* sender, MQDocument doc);
	BOOL EvenOffsetChanged(MQWidgetBase* sender, MQDocument doc);
	BOOL ApplyClicked(MQWidgetBase* sender, MQDocument doc);
	BOOL CancelClicked(MQWidgetBase* sender, MQDocument doc);

	void SyncFromPlugin();

private:
	InsetFacePlugin* m_Plugin;
	MQDoubleSpinBox* m_Thickness;
	MQDoubleSpinBox* m_Depth;
	MQRadioButton* m_RegionMode;
	MQRadioButton* m_IndividualMode;
	MQCheckBox* m_EvenOffset;
	MQButton* m_ApplyButton;
	MQButton* m_CancelButton;
	bool m_Syncing;
};

class InsetFacePlugin : public MQCommandPlugin
{
	friend class InsetFaceWindow;

public:
	InsetFacePlugin();

	void GetPlugInID(DWORD* Product, DWORD* ID) override;
	const char* GetPlugInName(void) override;
	const wchar_t* EnumString(void) override;
	BOOL Initialize() override;
	void Exit() override;
	BOOL Activate(MQDocument doc, BOOL flag) override;
	void OnDraw(MQDocument doc, MQScene scene, int width, int height) override;
	void OnObjectSelected(MQDocument doc) override;
	void OnUpdateScene(MQDocument doc, MQScene scene) override;
	BOOL OnLeftButtonDown(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state) override;
	BOOL OnLeftButtonMove(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state) override;
	BOOL OnLeftButtonUp(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state) override;
	BOOL OnMouseMove(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state) override;
	BOOL OnKeyDown(MQDocument doc, MQScene scene, int key, MOUSE_BUTTON_STATE& state) override;
	void LoadResource();
	std::wstring LocalizedText(const char* key, const wchar_t* fallback = L"") const;
	void AppendLocalizedWarning(std::wstring& target, const char* key) const;

private:
	enum class ApplySource
	{
		Button,
		Keyboard,
		Drag,
	};

	struct DragState
	{
		bool Pending;
		bool Active;
		bool Moved;
		bool PreserveSelectionForDrag;
		BOOL ShiftAtMouseDown;
		int HitObjectIndex;
		UINT HitFaceUniqueID;
		POINT StartPoint;
		double StartThickness;

		DragState()
			: Pending(false)
			, Active(false)
			, Moved(false)
			, PreserveSelectionForDrag(false)
			, ShiftAtMouseDown(FALSE)
			, HitObjectIndex(-1)
			, HitFaceUniqueID(0)
			, StartThickness(0.0)
		{
			StartPoint.x = 0;
			StartPoint.y = 0;
		}
	};

	bool m_Activated;
	bool m_PreviewDirty;
	bool m_HoverFaceValid;
	int m_HoverObjectIndex;
	UINT m_HoverFaceUniqueID;
	InsetParameters m_Params;
	PreviewMesh m_Preview;
	std::set<SelectedFaceRef> m_SelectedFaces;
	MQSelectOperation m_SelectOperation;
	MLanguage m_Language;
	DragState m_Drag;
	HCURSOR m_MoveCursor;
	InsetFaceWindow* m_Window;
	std::wstring m_LastWarning;

	void ResetToolState(bool clear_selection);
	void InvalidatePreview();
	void SetStatus();
	void CancelCurrentOperation();
	void SyncSelectionFromDocument(MQDocument doc);
	bool RefreshSelectionFromDocument(MQDocument doc);
	void ClearDocumentSelection(MQDocument doc);
	void SyncDocumentSelection(MQDocument doc, int preferred_object_index = -1);
	bool IsFaceSelected(int object_index, UINT face_unique_id) const;
	void ResolveClickSelection(MQDocument doc, int object_index, UINT face_unique_id, bool shift_pressed);
	void ResetAfterApply(MQDocument doc, ApplySource source);

	bool EnsurePreview(MQDocument doc);
	bool RebuildPreview(MQDocument doc);
	bool BuildFaceInfoList(MQDocument doc, int object_index, std::vector<FaceInfo>& out_faces) const;
	void BuildFaceGroups(const std::vector<FaceInfo>& faces, std::vector<std::vector<FaceInfo> >& groups) const;
	bool BuildPreviewForFaceGroup(MQDocument doc, int object_index, const std::vector<FaceInfo>& faces, PreviewObject& preview_object, RegionCommitData& commit_data);
	void AppendPreviewLinesFromRegion(const TempRegion& region, PreviewObject& preview_object) const;
	MQColor GetFaceBaseColor(MQDocument doc, MQObject obj, int face_index);
	MQColor MixColor(const MQColor& base, const MQColor& highlight, double highlight_weight) const;

	bool BuildTempRegion(MQObject obj, int object_index, const std::vector<FaceInfo>& faces, TempRegion& region);
	void ClassifyBoundaryElements(TempRegion& region);
	bool BuildFaceLocalInset(TempRegion& region, TempFace& face);
	bool SolvePatchInsetVertices(TempRegion& region, double depth);
	bool FitLocalPlane(TempRegion& region, const std::vector<FaceInfo>& faces);
	bool IsRegionNearlyPlanar(const TempRegion& region, const std::vector<FaceInfo>& faces, double thickness) const;
	void ProjectRegionVertices(TempRegion& region);
	bool ExtractBoundaryLoops(TempRegion& region);
	bool OffsetLoop2D(const std::vector<Point2D>& loop_points, double signed_thickness, bool even_offset, std::vector<Point2D>& out_points, std::wstring& warning, bool allow_clamp = true) const;
	bool SolveRegionInterior2D(TempRegion& region, double thickness, double depth, bool even_offset);
	bool BuildSurfaceAwareBoundaryTargets(TempRegion& region, double thickness, bool even_offset);
	bool SolveRegionInterior3D(TempRegion& region, double depth);
	bool SolveRegionPlanar(TempRegion& region, double thickness, double depth, bool even_offset);
	bool SolveRegionSurfaceAware(TempRegion& region, double thickness, double depth, bool even_offset);
	MQPoint LiftPoint(const TempRegion& region, const Point2D& point) const;

	bool ApplyPreview(MQDocument doc, ApplySource source = ApplySource::Button);
	bool ApplyCommitDataToObject(MQDocument doc, MQObject obj, const RegionCommitData& commit_data);

	static MQPoint ComputeFaceNormal(MQObject obj, const std::vector<int>& vertices);
	static MQPoint ComputeVertexNormal(MQObject obj, int vertex_index, const std::set<int>& region_faces);
	bool ComputeInsetPolygon(
		const std::vector<MQPoint>& polygon,
		const std::vector<MQCoordinate>& uv,
		double thickness,
		double depth,
		bool even_offset,
		std::vector<MQPoint>& inner_points,
		std::vector<MQCoordinate>& inner_uv,
		MQPoint* out_face_normal,
		std::wstring& warning);
};

InsetFaceWindow::InsetFaceWindow(int id, InsetFacePlugin* plugin)
	: MQWindow(id)
	, m_Plugin(plugin)
	, m_Syncing(false)
{
	m_Plugin->LoadResource();
	SetTitle(m_Plugin->LocalizedText("Title", L"Inset Face"));

	MQFrame* root = CreateVerticalFrame(this);
	root->SetOutSpace(0.2);

	MQFrame* thickness_frame = CreateHorizontalFrame(root);
	CreateLabel(thickness_frame, m_Plugin->LocalizedText("Thickness", L"Thickness"));
	m_Thickness = CreateDoubleSpinBox(thickness_frame);
	m_Thickness->SetMin(-1e8);
	m_Thickness->SetMax(1e8);
	m_Thickness->SetPosition(4);
	m_Thickness->SetDisplayUnit(plugin->GetDisplayUnit());
	m_Thickness->AddChangedEvent(this, &InsetFaceWindow::ThicknessChanged);
	m_Thickness->AddChangingEvent(this, &InsetFaceWindow::ThicknessChanged);

	MQFrame* depth_frame = CreateHorizontalFrame(root);
	CreateLabel(depth_frame, m_Plugin->LocalizedText("Depth", L"Depth"));
	m_Depth = CreateDoubleSpinBox(depth_frame);
	m_Depth->SetMin(-1e8);
	m_Depth->SetMax(1e8);
	m_Depth->SetPosition(4);
	m_Depth->SetDisplayUnit(plugin->GetDisplayUnit());
	m_Depth->AddChangedEvent(this, &InsetFaceWindow::DepthChanged);
	m_Depth->AddChangingEvent(this, &InsetFaceWindow::DepthChanged);

	MQFrame* mode_frame = CreateHorizontalFrame(root);
	CreateLabel(mode_frame, m_Plugin->LocalizedText("Mode", L"Mode"));
	m_RegionMode = CreateRadioButton(mode_frame, m_Plugin->LocalizedText("ModeRegion", L"Region"));
	m_IndividualMode = CreateRadioButton(mode_frame, m_Plugin->LocalizedText("ModeIndividual", L"Individual"));
	m_RegionMode->AddChangedEvent(this, &InsetFaceWindow::RegionModeChanged);
	m_IndividualMode->AddChangedEvent(this, &InsetFaceWindow::IndividualModeChanged);

	MQFrame* option_frame = CreateHorizontalFrame(root);
	m_EvenOffset = CreateCheckBox(option_frame, m_Plugin->LocalizedText("EvenOffset", L"Offset Even"));
	m_EvenOffset->AddChangedEvent(this, &InsetFaceWindow::EvenOffsetChanged);

	MQFrame* button_frame = CreateHorizontalFrame(root);
	button_frame->SetUniformSize(true);
	m_ApplyButton = CreateButton(button_frame, m_Plugin->LocalizedText("Apply", L"Apply"));
	m_ApplyButton->AddClickEvent(this, &InsetFaceWindow::ApplyClicked);
	m_CancelButton = CreateButton(button_frame, m_Plugin->LocalizedText("Cancel", L"Cancel"));
	m_CancelButton->AddClickEvent(this, &InsetFaceWindow::CancelClicked);

	SyncFromPlugin();
}

InsetFaceWindow::~InsetFaceWindow()
{
}

void InsetFaceWindow::SyncFromPlugin()
{
	m_Syncing = true;
	m_Thickness->SetPosition(m_Plugin->m_Params.Thickness);
	m_Depth->SetPosition(m_Plugin->m_Params.Depth);
	m_RegionMode->SetChecked(m_Plugin->m_Params.CurrentMode == InsetParameters::ModeRegion);
	m_IndividualMode->SetChecked(m_Plugin->m_Params.CurrentMode == InsetParameters::ModeIndividual);
	m_EvenOffset->SetChecked(m_Plugin->m_Params.EvenOffset);
	m_Syncing = false;
}

BOOL InsetFaceWindow::ThicknessChanged(MQWidgetBase* sender, MQDocument doc)
{
	if (m_Syncing) return FALSE;
	m_Plugin->m_Params.Thickness = m_Thickness->GetPosition();
	m_Plugin->InvalidatePreview();
	m_Plugin->SetStatus();
	m_Plugin->RedrawAllScene();
	return FALSE;
}

BOOL InsetFaceWindow::DepthChanged(MQWidgetBase* sender, MQDocument doc)
{
	if (m_Syncing) return FALSE;
	m_Plugin->m_Params.Depth = m_Depth->GetPosition();
	m_Plugin->InvalidatePreview();
	m_Plugin->SetStatus();
	m_Plugin->RedrawAllScene();
	return FALSE;
}

BOOL InsetFaceWindow::RegionModeChanged(MQWidgetBase* sender, MQDocument doc)
{
	if (m_Syncing) return FALSE;
	if (!m_RegionMode->GetChecked()) return FALSE;
	m_IndividualMode->SetChecked(false);
	m_Plugin->m_Params.CurrentMode = InsetParameters::ModeRegion;
	m_Plugin->InvalidatePreview();
	m_Plugin->SetStatus();
	m_Plugin->RedrawAllScene();
	return FALSE;
}

BOOL InsetFaceWindow::IndividualModeChanged(MQWidgetBase* sender, MQDocument doc)
{
	if (m_Syncing) return FALSE;
	if (!m_IndividualMode->GetChecked()) return FALSE;
	m_RegionMode->SetChecked(false);
	m_Plugin->m_Params.CurrentMode = InsetParameters::ModeIndividual;
	m_Plugin->InvalidatePreview();
	m_Plugin->SetStatus();
	m_Plugin->RedrawAllScene();
	return FALSE;
}

BOOL InsetFaceWindow::EvenOffsetChanged(MQWidgetBase* sender, MQDocument doc)
{
	if (m_Syncing) return FALSE;
	m_Plugin->m_Params.EvenOffset = m_EvenOffset->GetChecked();
	m_Plugin->InvalidatePreview();
	m_Plugin->SetStatus();
	m_Plugin->RedrawAllScene();
	return FALSE;
}

BOOL InsetFaceWindow::ApplyClicked(MQWidgetBase* sender, MQDocument doc)
{
	m_Plugin->ApplyPreview(doc, InsetFacePlugin::ApplySource::Button);
	return TRUE;
}

BOOL InsetFaceWindow::CancelClicked(MQWidgetBase* sender, MQDocument doc)
{
	m_Plugin->CancelCurrentOperation();
	return TRUE;
}

InsetFacePlugin::InsetFacePlugin()
	: m_Activated(false)
	, m_PreviewDirty(true)
	, m_HoverFaceValid(false)
	, m_HoverObjectIndex(-1)
	, m_HoverFaceUniqueID(0)
	, m_MoveCursor(NULL)
	, m_Window(NULL)
{
}

void InsetFacePlugin::LoadResource()
{
	if (m_Language.Contains()) return;

	std::wstring language = NormalizeLanguageName(GetSettingValue(MQSETTINGVALUE_LANGUAGE));
	std::wstring resource_path = JoinPath(GetPluginModuleDirectory(), kResourceFileName);
	if (!m_Language.Load(language, resource_path)) {
		m_Language.Load(L"English", resource_path);
	}
}

std::wstring InsetFacePlugin::LocalizedText(const char* key, const wchar_t* fallback) const
{
	const wchar_t* localized = m_Language.Search(key);
	if (localized != NULL && localized[0] != L'\0') {
		return localized;
	}
	return fallback != NULL ? std::wstring(fallback) : std::wstring();
}

void InsetFacePlugin::AppendLocalizedWarning(std::wstring& target, const char* key) const
{
	AppendWarning(target, LocalizedText(key).c_str());
}

void InsetFacePlugin::GetPlugInID(DWORD* Product, DWORD* ID)
{
	*Product = kInsetFaceProductId;
	*ID = kInsetFacePluginId;
}

const char* InsetFacePlugin::GetPlugInName(void)
{
	return "Inset Face       Copyright(C) 2026";
}

const wchar_t* InsetFacePlugin::EnumString(void)
{
	LoadResource();
	static std::wstring menu_name;
	menu_name = LocalizedText("MenuName", L"Inset Face");
	return menu_name.c_str();
}

BOOL InsetFacePlugin::Initialize()
{
	LoadResource();
	m_MoveCursor = GetResourceCursor(MQCURSOR_MOVE);
	return TRUE;
}

void InsetFacePlugin::Exit()
{
	delete m_Window;
	m_Window = NULL;
}

BOOL InsetFacePlugin::Activate(MQDocument doc, BOOL flag)
{
	if (flag) {
		LoadResource();
		m_Params.EvenOffset = false;
		ResetToolState(false);
		SyncSelectionFromDocument(doc);
		m_Window = new InsetFaceWindow(MQWindow::GetSystemWidgetID(MQSystemWidget::OptionPanel), this);
		POINT size = m_Window->GetJustSize();
		m_Window->SetWidth(size.x);
		m_Window->SetHeight(size.y);
		m_Window->SetVisible(true);
		SetStatus();
	}
	else {
		delete m_Window;
		m_Window = NULL;
		ResetToolState(false);
		RedrawAllScene();
	}

	m_Activated = flag ? true : false;
	return flag;
}

void InsetFacePlugin::OnObjectSelected(MQDocument doc)
{
	if (!m_Activated) return;
	SyncSelectionFromDocument(doc);
	RedrawAllScene();
}

void InsetFacePlugin::OnUpdateScene(MQDocument doc, MQScene scene)
{
	if (!m_Activated) return;
	if (m_Drag.Active || m_Drag.Pending) return;
	if (RefreshSelectionFromDocument(doc)) {
		RedrawScene(scene);
	}
}

void InsetFacePlugin::ResetToolState(bool clear_selection)
{
	m_Drag = DragState();
	m_SelectOperation.Clear();
	m_HoverFaceValid = false;
	m_HoverObjectIndex = -1;
	m_HoverFaceUniqueID = 0;
	m_Preview.Clear();
	m_PreviewDirty = true;
	m_LastWarning.clear();
	if (clear_selection) {
		m_SelectedFaces.clear();
	}
}

void InsetFacePlugin::InvalidatePreview()
{
	m_Preview.Clear();
	m_PreviewDirty = true;
}

void InsetFacePlugin::SetStatus()
{
	LoadResource();
	wchar_t text[512];
	const std::wstring region_mode = LocalizedText("ModeRegion", L"Region");
	const std::wstring individual_mode = LocalizedText("ModeIndividual", L"Individual");
	const wchar_t* mode_text = m_Params.CurrentMode == InsetParameters::ModeRegion ? region_mode.c_str() : individual_mode.c_str();
	if (m_LastWarning.empty()) {
		const std::wstring format = LocalizedText("StatusFormat", L"Inset Face  Faces:%u  Thickness: %.3f  Depth: %.3f  Mode:%s");
		swprintf_s(text, format.c_str(),
			(unsigned)m_SelectedFaces.size(),
			m_Params.Thickness,
			m_Params.Depth,
			mode_text);
	}
	else {
		const std::wstring format = LocalizedText("StatusWarningFormat", L"Inset Face  Faces:%u  Thickness: %.3f  Depth: %.3f  Mode:%s  Warning:%s");
		swprintf_s(text, format.c_str(),
			(unsigned)m_SelectedFaces.size(),
			m_Params.Thickness,
			m_Params.Depth,
			mode_text,
			m_LastWarning.c_str());
	}
	SetStatusString(text);
}

void InsetFacePlugin::CancelCurrentOperation()
{
	m_Drag = DragState();
	InvalidatePreview();
	m_LastWarning.clear();
	SetStatus();
	RedrawAllScene();
}

void InsetFacePlugin::SyncSelectionFromDocument(MQDocument doc)
{
	if (!RefreshSelectionFromDocument(doc)) {
		SetStatus();
	}
}

bool InsetFacePlugin::RefreshSelectionFromDocument(MQDocument doc)
{
	std::set<SelectedFaceRef> new_selection;
	int object_count = doc->GetObjectCount();
	for (int oi = 0; oi < object_count; ++oi) {
		MQObject obj = doc->GetObject(oi);
		if (obj == NULL) continue;
		int face_count = obj->GetFaceCount();
		for (int fi = 0; fi < face_count; ++fi) {
			if (obj->GetFacePointCount(fi) >= 3 && doc->IsSelectFace(oi, fi)) {
				new_selection.insert(SelectedFaceRef(oi, obj->GetFaceUniqueID(fi)));
			}
		}
	}

	if (new_selection == m_SelectedFaces) {
		return false;
	}

	m_SelectedFaces.swap(new_selection);
	InvalidatePreview();
	SetStatus();
	return true;
}

void InsetFacePlugin::ClearDocumentSelection(MQDocument doc)
{
	int object_count = doc->GetObjectCount();
	for (int oi = 0; oi < object_count; ++oi) {
		MQObject obj = doc->GetObject(oi);
		if (obj == NULL) continue;
		int face_count = obj->GetFaceCount();
		for (int fi = 0; fi < face_count; ++fi) {
			if (doc->IsSelectFace(oi, fi)) {
				doc->DeleteSelectFace(oi, fi);
			}
		}
	}
}

void InsetFacePlugin::SyncDocumentSelection(MQDocument doc, int preferred_object_index)
{
	// Callers have already changed the local selection; document comparison
	// cannot detect that change after it has been synchronized.
	InvalidatePreview();
	if (preferred_object_index >= 0) {
		doc->SetCurrentObjectIndex(preferred_object_index);
	}
	else if (!m_SelectedFaces.empty()) {
		doc->SetCurrentObjectIndex(m_SelectedFaces.begin()->ObjectIndex);
	}

	ClearDocumentSelection(doc);
	for (std::set<SelectedFaceRef>::const_iterator it = m_SelectedFaces.begin(); it != m_SelectedFaces.end(); ++it) {
		MQObject obj = doc->GetObject(it->ObjectIndex);
		if (obj == NULL) continue;
		int face_index = obj->GetFaceIndexFromUniqueID(it->FaceUniqueID);
		if (face_index >= 0) {
			doc->AddSelectFace(it->ObjectIndex, face_index);
		}
	}
}

bool InsetFacePlugin::IsFaceSelected(int object_index, UINT face_unique_id) const
{
	return m_SelectedFaces.find(SelectedFaceRef(object_index, face_unique_id)) != m_SelectedFaces.end();
}

void InsetFacePlugin::ResolveClickSelection(MQDocument doc, int object_index, UINT face_unique_id, bool shift_pressed)
{
	SelectedFaceRef ref(object_index, face_unique_id);
	std::set<SelectedFaceRef>::iterator it = m_SelectedFaces.find(ref);
	if (shift_pressed) {
		if (it != m_SelectedFaces.end()) {
			m_SelectedFaces.erase(it);
		}
		else {
			m_SelectedFaces.insert(ref);
		}
	}
	else {
		m_SelectedFaces.clear();
		m_SelectedFaces.insert(ref);
	}
	SyncDocumentSelection(doc, object_index);
	SyncSelectionFromDocument(doc);
	m_LastWarning.clear();
}

bool InsetFacePlugin::EnsurePreview(MQDocument doc)
{
	if (!m_PreviewDirty && m_Preview.Valid) return m_Preview.Complete;
	return RebuildPreview(doc);
}

bool InsetFacePlugin::BuildFaceInfoList(MQDocument doc, int object_index, std::vector<FaceInfo>& out_faces) const
{
	out_faces.clear();
	MQObject obj = doc->GetObject(object_index);
	if (obj == NULL || !obj->GetVisible() || obj->GetLocking()) return true;

	for (std::set<SelectedFaceRef>::const_iterator it = m_SelectedFaces.begin(); it != m_SelectedFaces.end(); ++it) {
		if (it->ObjectIndex != object_index) continue;
		int face_index = obj->GetFaceIndexFromUniqueID(it->FaceUniqueID);
		if (face_index < 0) continue;
		int point_count = obj->GetFacePointCount(face_index);
		if (point_count < 3) continue;

		FaceInfo info;
		info.FaceIndex = face_index;
		info.FaceUniqueID = it->FaceUniqueID;
		info.MaterialIndex = obj->GetFaceMaterial(face_index);
		info.Vertices.resize(point_count);
		info.UV.resize(point_count);
		obj->GetFacePointArray(face_index, info.Vertices.data());
		obj->GetFaceCoordinateArray(face_index, info.UV.data());
		info.FaceNormal = ComputeFaceNormal(obj, info.Vertices);
		out_faces.push_back(info);
	}

	return true;
}

void InsetFacePlugin::BuildFaceGroups(const std::vector<FaceInfo>& faces, std::vector<std::vector<FaceInfo> >& groups) const
{
	groups.clear();
	if (faces.empty()) return;

	if (m_Params.CurrentMode == InsetParameters::ModeIndividual) {
		for (size_t i = 0; i < faces.size(); ++i) {
			std::vector<FaceInfo> group;
			group.push_back(faces[i]);
			groups.push_back(group);
		}
		return;
	}

	std::map<EdgeKey, std::vector<int> > edge_to_faces;
	for (size_t fi = 0; fi < faces.size(); ++fi) {
		for (size_t vi = 0; vi < faces[fi].Vertices.size(); ++vi) {
			int v0 = faces[fi].Vertices[vi];
			int v1 = faces[fi].Vertices[(vi + 1) % faces[fi].Vertices.size()];
			edge_to_faces[EdgeKey(v0, v1)].push_back((int)fi);
		}
	}

	std::vector<std::vector<int> > adjacency(faces.size());
	for (std::map<EdgeKey, std::vector<int> >::const_iterator it = edge_to_faces.begin(); it != edge_to_faces.end(); ++it) {
		const std::vector<int>& edge_faces = it->second;
		for (size_t i = 0; i < edge_faces.size(); ++i) {
			for (size_t j = i + 1; j < edge_faces.size(); ++j) {
				adjacency[edge_faces[i]].push_back(edge_faces[j]);
				adjacency[edge_faces[j]].push_back(edge_faces[i]);
			}
		}
	}

	std::vector<bool> visited(faces.size(), false);
	for (size_t start = 0; start < faces.size(); ++start) {
		if (visited[start]) continue;
		std::vector<int> stack(1, (int)start);
		visited[start] = true;
		std::vector<FaceInfo> group;
		while (!stack.empty()) {
			int current = stack.back();
			stack.pop_back();
			group.push_back(faces[current]);
			for (size_t i = 0; i < adjacency[current].size(); ++i) {
				int next = adjacency[current][i];
				if (!visited[next]) {
					visited[next] = true;
					stack.push_back(next);
				}
			}
		}
		groups.push_back(group);
	}
}

MQPoint InsetFacePlugin::ComputeFaceNormal(MQObject obj, const std::vector<int>& vertices)
{
	if (vertices.size() < 3) return MQPoint(0, 0, 1);
	std::vector<MQPoint> polygon(vertices.size());
	for (size_t i = 0; i < vertices.size(); ++i) {
		polygon[i] = obj->GetVertex(vertices[i]);
	}

	MQPoint normal(0, 0, 0);
	for (size_t i = 0; i < polygon.size(); ++i) {
		const MQPoint& a = polygon[i];
		const MQPoint& b = polygon[(i + 1) % polygon.size()];
		normal.x += (a.y - b.y) * (a.z + b.z);
		normal.y += (a.z - b.z) * (a.x + b.x);
		normal.z += (a.x - b.x) * (a.y + b.y);
	}

	float len2 = GetInnerProduct(normal, normal);
	if (len2 <= 1e-12f) {
		const MQPoint& p0 = polygon[0];
		for (size_t i = 1; i + 1 < polygon.size(); ++i) {
			MQPoint n = GetNormal(p0, polygon[i], polygon[i + 1]);
			if (GetInnerProduct(n, n) > 1e-12f) return Normalize(n);
		}
		return MQPoint(0, 0, 1);
	}
	return Normalize(normal);
}

MQPoint InsetFacePlugin::ComputeVertexNormal(MQObject obj, int vertex_index, const std::set<int>& region_faces)
{
	int rel_count = obj->GetVertexRelatedFaces(vertex_index, NULL);
	if (rel_count <= 0) return MQPoint(0, 0, 1);

	std::vector<int> related_faces(rel_count);
	obj->GetVertexRelatedFaces(vertex_index, related_faces.data());

	MQPoint normal(0, 0, 0);
	for (int i = 0; i < rel_count; ++i) {
		int face_index = related_faces[i];
		if (region_faces.find(face_index) == region_faces.end()) continue;
		int point_count = obj->GetFacePointCount(face_index);
		if (point_count < 3) continue;
		std::vector<int> indices(point_count);
		obj->GetFacePointArray(face_index, indices.data());
		normal += ComputeFaceNormal(obj, indices);
	}

	float len2 = GetInnerProduct(normal, normal);
	if (len2 < 1e-12f) return MQPoint(0, 0, 1);
	return Normalize(normal);
}

bool InsetFacePlugin::ComputeInsetPolygon(
	const std::vector<MQPoint>& polygon,
	const std::vector<MQCoordinate>& uv,
	double thickness,
	double depth,
	bool even_offset,
	std::vector<MQPoint>& inner_points,
	std::vector<MQCoordinate>& inner_uv,
	MQPoint* out_face_normal,
	std::wstring& warning)
{
	inner_points.clear();
	inner_uv.clear();
	if (polygon.size() < 3 || uv.size() != polygon.size()) return false;

	MQPoint face_normal(0, 0, 0);
	for (size_t i = 0; i < polygon.size(); ++i) {
		const MQPoint& a = polygon[i];
		const MQPoint& b = polygon[(i + 1) % polygon.size()];
		face_normal.x += (a.y - b.y) * (a.z + b.z);
		face_normal.y += (a.z - b.z) * (a.x + b.x);
		face_normal.z += (a.x - b.x) * (a.y + b.y);
	}
	if (GetInnerProduct(face_normal, face_normal) <= 1e-12f) return false;
	face_normal = Normalize(face_normal);
	if (out_face_normal) *out_face_normal = face_normal;

	MQPoint origin = polygon[0];
	MQPoint plane_u = polygon[1] - polygon[0];
	plane_u -= face_normal * GetInnerProduct(plane_u, face_normal);
	if (GetInnerProduct(plane_u, plane_u) <= 1e-12f) {
		plane_u = MQPoint(1, 0, 0) - face_normal * GetInnerProduct(MQPoint(1, 0, 0), face_normal);
	}
	if (GetInnerProduct(plane_u, plane_u) <= 1e-12f) {
		plane_u = MQPoint(0, 1, 0) - face_normal * GetInnerProduct(MQPoint(0, 1, 0), face_normal);
	}
	plane_u = Normalize(plane_u);
	MQPoint plane_v = Normalize(GetCrossProduct(face_normal, plane_u));

	std::vector<Point2D> polygon_2d(polygon.size());
	for (size_t i = 0; i < polygon.size(); ++i) {
		MQPoint offset = polygon[i] - origin;
		polygon_2d[i] = Point2D(GetInnerProduct(offset, plane_u), GetInnerProduct(offset, plane_v));
	}

	std::vector<Point2D> inset_2d;
	if (!OffsetLoop2D(polygon_2d, thickness, even_offset, inset_2d, warning)) {
		return false;
	}

	inner_points.resize(inset_2d.size());
	inner_uv.resize(uv.size());
	MQPoint centroid(0, 0, 0);
	for (size_t i = 0; i < polygon.size(); ++i) centroid += polygon[i];
	centroid /= (float)polygon.size();
	MQCoordinate uv_centroid(0, 0);
	for (size_t i = 0; i < uv.size(); ++i) {
		uv_centroid.u += uv[i].u;
		uv_centroid.v += uv[i].v;
	}
	uv_centroid.u /= (float)uv.size();
	uv_centroid.v /= (float)uv.size();

	for (size_t i = 0; i < inset_2d.size(); ++i) {
		inner_points[i] = origin + plane_u * (float)inset_2d[i].X + plane_v * (float)inset_2d[i].Y + face_normal * (float)depth;
		MQCoordinate toward_uv(uv_centroid.u - uv[i].u, uv_centroid.v - uv[i].v);
		inner_uv[i].u = uv[i].u + toward_uv.u * 0.5f;
		inner_uv[i].v = uv[i].v + toward_uv.v * 0.5f;
	}
	return true;
}

static MQPoint ProjectVectorToPlane(const MQPoint& vector, const MQPoint& normal)
{
	return vector - normal * GetInnerProduct(vector, normal);
}

static double ComputeFaceCornerWeight(const std::vector<MQPoint>& polygon, size_t vertex_index, const MQPoint& face_normal)
{
	if (polygon.size() < 3) return 1.0;
	const MQPoint& prev = polygon[(vertex_index + polygon.size() - 1) % polygon.size()];
	const MQPoint& curr = polygon[vertex_index];
	const MQPoint& next = polygon[(vertex_index + 1) % polygon.size()];

	MQPoint edge_prev = ProjectVectorToPlane(prev - curr, face_normal);
	MQPoint edge_next = ProjectVectorToPlane(next - curr, face_normal);
	float prev_len2 = GetInnerProduct(edge_prev, edge_prev);
	float next_len2 = GetInnerProduct(edge_next, edge_next);
	if (prev_len2 <= 1e-12f || next_len2 <= 1e-12f) return 1.0;

	double angle = GetCrossingAngle(edge_prev, edge_next);
	if (!std::isfinite(angle) || angle <= kGeometryEpsilon) return 1.0;
	return angle;
}

void InsetFacePlugin::ClassifyBoundaryElements(TempRegion& region)
{
	for (size_t vi = 0; vi < region.Vertices.size(); ++vi) {
		region.Vertices[vi].Boundary = false;
	}
	for (size_t hi = 0; hi < region.HalfEdges.size(); ++hi) {
		region.HalfEdges[hi].Boundary = (region.HalfEdges[hi].Pair == -1);
		if (region.HalfEdges[hi].Boundary) {
			region.Vertices[region.HalfEdges[hi].StartVertex].Boundary = true;
			region.Vertices[region.HalfEdges[hi].EndVertex].Boundary = true;
		}
	}
}

bool InsetFacePlugin::BuildFaceLocalInset(TempRegion& region, TempFace& face)
{
	std::vector<MQPoint> polygon(face.Vertices.size());
	for (size_t i = 0; i < face.Vertices.size(); ++i) {
		polygon[i] = region.Vertices[face.Vertices[i]].OriginalPosition;
	}

	std::vector<MQCoordinate> inner_uv;
	MQPoint face_normal;
	if (!ComputeInsetPolygon(polygon, face.UV, m_Params.Thickness, 0.0, m_Params.EvenOffset, face.LocalInsetPoints, inner_uv, &face_normal, region.Warning)) {
		return false;
	}

	face.FaceNormal = face_normal;
	return true;
}

bool InsetFacePlugin::SolvePatchInsetVertices(TempRegion& region, double depth)
{
	std::vector<MQPoint> tangent_sum(region.Vertices.size(), MQPoint(0, 0, 0));
	std::vector<double> weight_sum(region.Vertices.size(), 0.0);

	for (size_t fi = 0; fi < region.Faces.size(); ++fi) {
		TempFace& face = region.Faces[fi];
		if (face.LocalInsetPoints.size() != face.Vertices.size()) {
			return false;
		}

		std::vector<MQPoint> polygon(face.Vertices.size());
		for (size_t vi = 0; vi < face.Vertices.size(); ++vi) {
			polygon[vi] = region.Vertices[face.Vertices[vi]].OriginalPosition;
		}

		for (size_t vi = 0; vi < face.Vertices.size(); ++vi) {
			int temp_vertex_index = face.Vertices[vi];
			TempVertex& temp_vertex = region.Vertices[temp_vertex_index];
			MQPoint average_normal = temp_vertex.AverageNormal;
			if (GetInnerProduct(average_normal, average_normal) <= 1e-12f) {
				average_normal = face.FaceNormal;
			}
			if (GetInnerProduct(average_normal, average_normal) <= 1e-12f) {
				average_normal = MQPoint(0, 0, 1);
			}
			average_normal = Normalize(average_normal);

			MQPoint local_offset = face.LocalInsetPoints[vi] - temp_vertex.OriginalPosition;
			MQPoint tangent_offset = ProjectVectorToPlane(local_offset, average_normal);
			double weight = ComputeFaceCornerWeight(polygon, vi, face.FaceNormal);
			tangent_sum[temp_vertex_index] += tangent_offset * (float)weight;
			weight_sum[temp_vertex_index] += weight;
		}
	}

	for (size_t vi = 0; vi < region.Vertices.size(); ++vi) {
		TempVertex& vertex = region.Vertices[vi];
		MQPoint average_normal = vertex.AverageNormal;
		if (GetInnerProduct(average_normal, average_normal) <= 1e-12f) {
			average_normal = MQPoint(0, 0, 1);
		}
		average_normal = Normalize(average_normal);

		MQPoint tangent_offset(0, 0, 0);
		if (weight_sum[vi] > kGeometryEpsilon) {
			tangent_offset = tangent_sum[vi] / (float)weight_sum[vi];
		}
		vertex.NewPosition = vertex.OriginalPosition + tangent_offset + average_normal * (float)depth;
	}

	return true;
}

bool InsetFacePlugin::FitLocalPlane(TempRegion& region, const std::vector<FaceInfo>& faces)
{
	MQPoint centroid(0, 0, 0);
	for (size_t i = 0; i < region.Vertices.size(); ++i) {
		centroid += region.Vertices[i].OriginalPosition;
	}
	if (!region.Vertices.empty()) {
		centroid /= (float)region.Vertices.size();
	}
	region.PlaneOrigin = centroid;

	MQPoint normal(0, 0, 0);
	for (size_t fi = 0; fi < faces.size(); ++fi) {
		std::vector<MQPoint> polygon(faces[fi].Vertices.size());
		for (size_t vi = 0; vi < faces[fi].Vertices.size(); ++vi) {
			for (size_t tv = 0; tv < region.Vertices.size(); ++tv) {
				if (region.Vertices[tv].OriginalVertexIndex == faces[fi].Vertices[vi]) {
					polygon[vi] = region.Vertices[tv].OriginalPosition;
					break;
				}
			}
		}
		double area = PolygonArea3D(polygon);
		normal += faces[fi].FaceNormal * (float)area;
	}

	if (GetInnerProduct(normal, normal) <= 1e-12f) {
		if (!faces.empty()) normal = faces[0].FaceNormal;
	}
	if (GetInnerProduct(normal, normal) <= 1e-12f) {
		AppendLocalizedWarning(region.Warning, "WarnZeroAreaFaceSkipped");
		return false;
	}
	region.PlaneNormal = Normalize(normal);

	MQPoint plane_u(0, 0, 0);
	double best_length = 0.0;
	for (size_t fi = 0; fi < region.Faces.size(); ++fi) {
		for (size_t vi = 0; vi < region.Faces[fi].Vertices.size(); ++vi) {
			const MQPoint& a = region.Vertices[region.Faces[fi].Vertices[vi]].OriginalPosition;
			const MQPoint& b = region.Vertices[region.Faces[fi].Vertices[(vi + 1) % region.Faces[fi].Vertices.size()]].OriginalPosition;
			MQPoint edge = b - a;
			edge -= region.PlaneNormal * GetInnerProduct(edge, region.PlaneNormal);
			double length = GetSize(edge);
			if (length > best_length) {
				best_length = length;
				plane_u = edge;
			}
		}
	}

	if (GetInnerProduct(plane_u, plane_u) <= 1e-12f) {
		plane_u = MQPoint(1, 0, 0) - region.PlaneNormal * GetInnerProduct(MQPoint(1, 0, 0), region.PlaneNormal);
	}
	if (GetInnerProduct(plane_u, plane_u) <= 1e-12f) {
		plane_u = MQPoint(0, 1, 0) - region.PlaneNormal * GetInnerProduct(MQPoint(0, 1, 0), region.PlaneNormal);
	}
	if (GetInnerProduct(plane_u, plane_u) <= 1e-12f) {
		AppendLocalizedWarning(region.Warning, "WarnPlaneFitFailed");
		return false;
	}

	region.PlaneU = Normalize(plane_u);
	region.PlaneV = Normalize(GetCrossProduct(region.PlaneNormal, region.PlaneU));
	return true;
}

bool InsetFacePlugin::IsRegionNearlyPlanar(const TempRegion& region, const std::vector<FaceInfo>& faces, double thickness) const
{
	const double effective_thickness = std::max(std::fabs(thickness), 1e-4);
	double max_vertex_plane_distance = 0.0;
	for (size_t i = 0; i < region.Vertices.size(); ++i) {
		const MQPoint delta = region.Vertices[i].OriginalPosition - region.PlaneOrigin;
		max_vertex_plane_distance = std::max(max_vertex_plane_distance, std::fabs((double)GetInnerProduct(delta, region.PlaneNormal)));
	}

	double max_face_normal_deviation_deg = 0.0;
	for (size_t fi = 0; fi < faces.size(); ++fi) {
		MQPoint face_normal = faces[fi].FaceNormal;
		if (GetInnerProduct(face_normal, face_normal) <= 1e-12f) {
			continue;
		}
		face_normal = Normalize(face_normal);
		double dot = std::max(-1.0, std::min(1.0, (double)GetInnerProduct(face_normal, region.PlaneNormal)));
		double angle_deg = std::acos(dot) * 180.0 / 3.14159265358979323846;
		max_face_normal_deviation_deg = std::max(max_face_normal_deviation_deg, angle_deg);
	}

	return max_vertex_plane_distance <= 0.25 * effective_thickness
		&& max_face_normal_deviation_deg <= 8.0;
}

void InsetFacePlugin::ProjectRegionVertices(TempRegion& region)
{
	for (size_t i = 0; i < region.Vertices.size(); ++i) {
		MQPoint delta = region.Vertices[i].OriginalPosition - region.PlaneOrigin;
		region.Vertices[i].OriginalProjected = Point2D(GetInnerProduct(delta, region.PlaneU), GetInnerProduct(delta, region.PlaneV));
		region.Vertices[i].NewProjected = region.Vertices[i].OriginalProjected;
		region.Vertices[i].NewPosition = region.Vertices[i].OriginalPosition;
	}
}

bool InsetFacePlugin::ExtractBoundaryLoops(TempRegion& region)
{
	std::vector<int> boundary_halfedges;
	std::map<int, std::vector<int> > outgoing_boundary;
	for (size_t hi = 0; hi < region.HalfEdges.size(); ++hi) {
		if (region.HalfEdges[hi].Pair == -1) {
			region.HalfEdges[hi].Boundary = true;
			boundary_halfedges.push_back((int)hi);
			outgoing_boundary[region.HalfEdges[hi].StartVertex].push_back((int)hi);
			region.Vertices[region.HalfEdges[hi].StartVertex].Boundary = true;
			region.Vertices[region.HalfEdges[hi].EndVertex].Boundary = true;
		}
	}

	if (boundary_halfedges.empty()) {
		AppendLocalizedWarning(region.Warning, "WarnNonManifoldSkipped");
		return false;
	}

	std::vector<bool> visited(region.HalfEdges.size(), false);
	for (size_t i = 0; i < boundary_halfedges.size(); ++i) {
		int start_halfedge = boundary_halfedges[i];
		if (visited[start_halfedge]) continue;

		TempLoop loop;
		int current = start_halfedge;
		int guard = 0;
		while (current >= 0 && !visited[current] && guard < (int)boundary_halfedges.size() + 8) {
			visited[current] = true;
			loop.HalfEdges.push_back(current);
			loop.Vertices.push_back(region.HalfEdges[current].StartVertex);

			int next_start = region.HalfEdges[current].EndVertex;
			const std::vector<int>& candidates = outgoing_boundary[next_start];
			int next_halfedge = -1;
			for (size_t ci = 0; ci < candidates.size(); ++ci) {
				if (!visited[candidates[ci]]) {
					next_halfedge = candidates[ci];
					break;
				}
			}
			if (next_halfedge == -1) {
				if (!candidates.empty() && candidates[0] == start_halfedge) {
					current = -1;
				}
				else {
					current = -1;
				}
			}
			else {
				current = next_halfedge;
			}
			++guard;
		}

		if (loop.Vertices.size() < 3) {
			AppendLocalizedWarning(region.Warning, "WarnDegenerateBoundarySkipped");
			return false;
		}
		region.BoundaryLoops.push_back(loop);
	}

	if (region.BoundaryLoops.empty()) {
		AppendLocalizedWarning(region.Warning, "WarnBoundaryExtractionFailed");
		return false;
	}

	for (size_t li = 0; li < region.BoundaryLoops.size(); ++li) {
		std::vector<Point2D> loop_points(region.BoundaryLoops[li].Vertices.size());
		for (size_t vi = 0; vi < region.BoundaryLoops[li].Vertices.size(); ++vi) {
			loop_points[vi] = region.Vertices[region.BoundaryLoops[li].Vertices[vi]].OriginalProjected;
		}
		region.BoundaryLoops[li].Area = PolygonArea2D(loop_points);
		region.BoundaryLoops[li].IsHole = region.BoundaryLoops[li].Area < 0.0;
	}

	return true;
}

bool InsetFacePlugin::OffsetLoop2D(const std::vector<Point2D>& loop_points, double signed_thickness, bool even_offset, std::vector<Point2D>& out_points, std::wstring& warning, bool allow_clamp) const
{
	out_points.clear();
	if (loop_points.size() < 3) return false;

	double source_area = PolygonArea2D(loop_points);
	if (std::fabs(source_area) <= kGeometryEpsilon) {
		AppendLocalizedWarning(warning, "WarnZeroAreaFaceSkipped");
		return false;
	}
	double orientation = source_area >= 0.0 ? 1.0 : -1.0;

	double scale = 1.0;
	for (int attempt = 0; attempt < (allow_clamp ? 24 : 1); ++attempt) {
		double distance = signed_thickness * scale;
		std::vector<Point2D> candidate(loop_points.size());
		bool had_parallel_fallback = false;
		bool valid = true;

		for (size_t i = 0; i < loop_points.size(); ++i) {
			const Point2D& prev = loop_points[(i + loop_points.size() - 1) % loop_points.size()];
			const Point2D& curr = loop_points[i];
			const Point2D& next = loop_points[(i + 1) % loop_points.size()];

			Point2D edge0 = curr - prev;
			Point2D edge1 = next - curr;
			double edge0_len = Length2D(edge0);
			double edge1_len = Length2D(edge1);
			if (edge0_len <= kGeometryEpsilon || edge1_len <= kGeometryEpsilon) {
				valid = false;
				AppendLocalizedWarning(warning, "WarnDegenerateEdgeSkipped");
				break;
			}

			Point2D dir0 = edge0 / edge0_len;
			Point2D dir1 = edge1 / edge1_len;
			Point2D inward0 = Normalize2D(PerpLeft(dir0) * orientation);
			Point2D inward1 = Normalize2D(PerpLeft(dir1) * orientation);
			Point2D bisector = inward0 + inward1;
			if (Length2D(bisector) <= kGeometryEpsilon) {
				bisector = inward1;
			}
			bisector = Normalize2D(bisector);

			Point2D intersection;
			if (!even_offset) {
				double bisector_alignment = Dot2D(bisector, inward1);
				if (bisector_alignment <= kGeometryEpsilon) {
					had_parallel_fallback = true;
					intersection = curr + inward1 * distance;
				}
				else {
					intersection = curr + bisector * distance;
				}
			}
			else {
				Point2D line0_point = curr + inward0 * distance;
				Point2D line1_point = curr + inward1 * distance;
				if (!IntersectInfiniteLines(line0_point, dir0, line1_point, dir1, intersection)) {
					had_parallel_fallback = true;
					double bisector_alignment = Dot2D(bisector, inward1);
					double miter_length = std::fabs(bisector_alignment) > kGeometryEpsilon ? distance / bisector_alignment : distance;
					double max_length = std::fabs(distance) * (kMaxMiterFactor * 1.5);
					if (std::fabs(miter_length) > max_length) {
						miter_length = (miter_length < 0.0 ? -max_length : max_length);
					}
					intersection = curr + bisector * miter_length;
				}
				else {
					Point2D delta = intersection - curr;
					double max_length = std::fabs(distance) * (kMaxMiterFactor * 1.5);
					if (max_length > 0.0 && Length2D(delta) > max_length) {
						Point2D dir = Normalize2D(delta);
						intersection = curr + dir * max_length;
						AppendLocalizedWarning(warning, "WarnParallelEdgeFallback");
					}
				}
			}

			candidate[i] = intersection;
		}

		if (!valid) {
			scale *= 0.5;
			continue;
		}

		double area = PolygonArea2D(candidate);
		if (std::fabs(area) <= kGeometryEpsilon || (area > 0.0) != (source_area > 0.0) || PolygonHasSelfIntersection(candidate) ||
			!BoundaryMotionIsValid2D(std::vector<std::vector<Point2D> >(1, loop_points), std::vector<std::vector<Point2D> >(1, candidate))) {
			scale *= 0.5;
			continue;
		}

		if (had_parallel_fallback) AppendLocalizedWarning(warning, "WarnParallelEdgeFallback");
		if (scale < 0.999) AppendLocalizedWarning(warning, "WarnCollapsedClamped");
		out_points = candidate;
		return true;
	}

	AppendLocalizedWarning(warning, "WarnCollapsed");
	return false;
}

MQPoint InsetFacePlugin::LiftPoint(const TempRegion& region, const Point2D& point) const
{
	return region.PlaneOrigin + region.PlaneU * (float)point.X + region.PlaneV * (float)point.Y;
}

bool InsetFacePlugin::SolveRegionInterior2D(TempRegion& region, double thickness, double depth, bool even_offset)
{
	std::vector<bool> fixed(region.Vertices.size(), false);
	std::vector<std::vector<int> > adjacency(region.Vertices.size());

	for (size_t hi = 0; hi < region.HalfEdges.size(); ++hi) {
		const TempHalfEdge& halfedge = region.HalfEdges[hi];
		adjacency[halfedge.StartVertex].push_back(halfedge.EndVertex);
		adjacency[halfedge.EndVertex].push_back(halfedge.StartVertex);
	}

	std::vector<std::vector<Point2D> > source_loops(region.BoundaryLoops.size());
	std::vector<std::vector<Point2D> > inset_loops(region.BoundaryLoops.size());
	for (size_t li = 0; li < region.BoundaryLoops.size(); ++li) {
		for (size_t vi = 0; vi < region.BoundaryLoops[li].Vertices.size(); ++vi) {
			source_loops[li].push_back(region.Vertices[region.BoundaryLoops[li].Vertices[vi]].OriginalProjected);
		}
	}
	bool boundary_valid = false;
	double boundary_scale = 1.0;
	for (int attempt = 0; attempt < 24; ++attempt, boundary_scale *= 0.5) {
		bool loops_valid = true;
		std::wstring attempt_warning;
		for (size_t li = 0; li < region.BoundaryLoops.size(); ++li) {
			double signed_thickness = (region.BoundaryLoops[li].IsHole ? -thickness : thickness) * boundary_scale;
			if (!OffsetLoop2D(source_loops[li], signed_thickness, even_offset, inset_loops[li], attempt_warning, false)) {
				loops_valid = false;
				break;
			}
		}
		// No contact during motion also preserves the initial containment of holes.
		if (loops_valid && BoundaryMotionIsValid2D(source_loops, inset_loops)) {
			AppendWarning(region.Warning, attempt_warning);
			if (boundary_scale < 0.999) AppendLocalizedWarning(region.Warning, "WarnCollapsedClamped");
			boundary_valid = true;
			break;
		}
	}
	if (!boundary_valid) {
		AppendLocalizedWarning(region.Warning, "WarnCollapsed");
		return false;
	}
	for (size_t li = 0; li < region.BoundaryLoops.size(); ++li) {
		for (size_t vi = 0; vi < region.BoundaryLoops[li].Vertices.size(); ++vi) {
			int vertex_index = region.BoundaryLoops[li].Vertices[vi];
			fixed[vertex_index] = true;
			region.Vertices[vertex_index].NewProjected = inset_loops[li][vi];
		}
	}

	for (int iteration = 0; iteration < kInteriorSolveIterations; ++iteration) {
		for (size_t vi = 0; vi < region.Vertices.size(); ++vi) {
			if (fixed[vi]) continue;
			if (adjacency[vi].empty()) continue;

			Point2D sum(0.0, 0.0);
			int count = 0;
			for (size_t ni = 0; ni < adjacency[vi].size(); ++ni) {
				const TempVertex& neighbor = region.Vertices[adjacency[vi][ni]];
				sum += neighbor.NewProjected - neighbor.OriginalProjected;
				++count;
			}
			if (count > 0) {
				region.Vertices[vi].NewProjected = region.Vertices[vi].OriginalProjected + sum / (double)count;
			}
		}
	}

	for (size_t vi = 0; vi < region.Vertices.size(); ++vi) {
		// Apply the solved displacement to retain any original height above the fitted plane.
		const Point2D displacement = region.Vertices[vi].NewProjected - region.Vertices[vi].OriginalProjected;
		region.Vertices[vi].NewPosition = region.Vertices[vi].OriginalPosition
			+ region.PlaneU * (float)displacement.X + region.PlaneV * (float)displacement.Y
			+ region.Vertices[vi].AverageNormal * (float)depth;
	}
	return true;
}

bool InsetFacePlugin::BuildSurfaceAwareBoundaryTargets(TempRegion& region, double thickness, bool even_offset)
{
	std::vector<int> incoming(region.Vertices.size(), -1), outgoing(region.Vertices.size(), -1);
	bool has_boundary = false;
	for (size_t hi = 0; hi < region.HalfEdges.size(); ++hi) {
		const TempHalfEdge& edge = region.HalfEdges[hi];
		if (!edge.Boundary) continue;
		has_boundary = true;
		if (outgoing[edge.StartVertex] != -1 || incoming[edge.EndVertex] != -1) {
			AppendLocalizedWarning(region.Warning, "WarnNonManifoldSkipped");
			return false;
		}
		outgoing[edge.StartVertex] = (int)hi;
		incoming[edge.EndVertex] = (int)hi;
	}
	if (!has_boundary) {
		AppendLocalizedWarning(region.Warning, "WarnBoundaryExtractionFailed");
		return false;
	}
	for (size_t vi = 0; vi < region.Vertices.size(); ++vi) {
		TempVertex& vertex = region.Vertices[vi];
		vertex.BoundaryTargetPosition = vertex.OriginalPosition;
		if (!vertex.Boundary) continue;
		if (incoming[vi] == -1 || outgoing[vi] == -1) {
			AppendLocalizedWarning(region.Warning, "WarnBoundaryExtractionFailed");
			return false;
		}
		MQPoint normal = vertex.AverageNormal;
		if (GetInnerProduct(normal, normal) <= 1e-12f) normal = MQPoint(0, 0, 1);
		normal = Normalize(normal);
		MQPoint inward[2];
		const int edges[2] = { incoming[vi], outgoing[vi] };
		for (int i = 0; i < 2; ++i) {
			const TempHalfEdge& edge = region.HalfEdges[edges[i]];
			const MQPoint direction = region.Vertices[edge.EndVertex].OriginalPosition
				- region.Vertices[edge.StartVertex].OriginalPosition;
			// Only exposed edges constrain the inset; selected internal seams do not.
			inward[i] = ProjectVectorToPlane(GetCrossProduct(region.Faces[edge.FaceIndex].FaceNormal, direction), normal);
			if (GetInnerProduct(inward[i], inward[i]) <= 1e-12f) {
				AppendLocalizedWarning(region.Warning, "WarnDegenerateBoundarySkipped");
				return false;
			}
			inward[i] = Normalize(inward[i]);
		}
		MQPoint displacement(0, 0, 0);
		if (!even_offset) {
			MQPoint bisector = inward[0] + inward[1];
			if (GetInnerProduct(bisector, bisector) <= 1e-12f) bisector = inward[0];
			displacement = Normalize(bisector) * (float)thickness;
			vertex.BoundaryTargetPosition = vertex.OriginalPosition + displacement;
			continue;
		}
		// Solve dot(d,inward0)=t, dot(d,inward1)=t, dot(d,normal)=0.
		MQPoint cross01 = GetCrossProduct(inward[1], normal);
		MQPoint cross12 = GetCrossProduct(normal, inward[0]);
		double determinant = GetInnerProduct(inward[0], cross01);
		if (std::fabs(determinant) > 1e-8) {
			displacement = (cross01 + cross12) * (float)(thickness / determinant);
		}
		else {
			MQPoint bisector = inward[0] + inward[1];
			if (GetInnerProduct(bisector, bisector) <= 1e-12f) {
				AppendLocalizedWarning(region.Warning, "WarnDegenerateBoundarySkipped");
				return false;
			}
			displacement = Normalize(bisector) * (float)thickness;
			AppendLocalizedWarning(region.Warning, "WarnParallelEdgeFallback");
		}
		displacement = ProjectVectorToPlane(displacement, normal);
		const double max_miter = std::fabs(thickness) * kMaxMiterFactor;
		const double displacement_length = GetSize(displacement);
		if (max_miter > 0.0 && displacement_length > max_miter) {
			displacement *= (float)(max_miter / displacement_length);
			AppendLocalizedWarning(region.Warning, "WarnParallelEdgeFallback");
		}
		if (!std::isfinite(displacement.x) || !std::isfinite(displacement.y) || !std::isfinite(displacement.z)) {
			AppendLocalizedWarning(region.Warning, "WarnDegenerateBoundarySkipped");
			return false;
		}
		vertex.BoundaryTargetPosition = vertex.OriginalPosition + displacement;
	}

	return true;
}

bool InsetFacePlugin::SolveRegionInterior3D(TempRegion& region, double depth)
{
	std::vector<bool> fixed(region.Vertices.size(), false);
	std::vector<std::vector<int> > adjacency(region.Vertices.size());
	std::vector<std::vector<std::pair<int, double> > > weighted_adjacency(region.Vertices.size());

	for (size_t hi = 0; hi < region.HalfEdges.size(); ++hi) {
		const TempHalfEdge& halfedge = region.HalfEdges[hi];
		adjacency[halfedge.StartVertex].push_back(halfedge.EndVertex);
		adjacency[halfedge.EndVertex].push_back(halfedge.StartVertex);
	}
	for (size_t vi = 0; vi < adjacency.size(); ++vi) {
		std::map<int, double> unique_weights;
		for (size_t ni = 0; ni < adjacency[vi].size(); ++ni) {
			const MQPoint delta = region.Vertices[adjacency[vi][ni]].OriginalPosition - region.Vertices[vi].OriginalPosition;
			double length = GetSize(delta);
			if (length > 1e-8 && std::isfinite(length)) unique_weights[adjacency[vi][ni]] += 1.0 / length;
		}
		for (std::map<int, double>::const_iterator it = unique_weights.begin(); it != unique_weights.end(); ++it) {
			weighted_adjacency[vi].push_back(std::make_pair(it->first, it->second));
		}
	}

	for (size_t vi = 0; vi < region.Vertices.size(); ++vi) {
		TempVertex& vertex = region.Vertices[vi];
		MQPoint average_normal = vertex.AverageNormal;
		if (GetInnerProduct(average_normal, average_normal) <= 1e-12f) {
			average_normal = MQPoint(0, 0, 1);
		}
		average_normal = Normalize(average_normal);

		vertex.SolvedTangentOffset = MQPoint(0, 0, 0);
		if (vertex.Boundary) {
			vertex.SolvedTangentOffset = ProjectVectorToPlane(vertex.BoundaryTargetPosition - vertex.OriginalPosition, average_normal);
			fixed[vi] = true;
		}
	}

	MQPoint min_point(0, 0, 0), max_point(0, 0, 0);
	if (!region.Vertices.empty()) min_point = max_point = region.Vertices[0].OriginalPosition;
	for (size_t vi = 1; vi < region.Vertices.size(); ++vi) {
		const MQPoint& p = region.Vertices[vi].OriginalPosition;
		min_point.x = std::min(min_point.x, p.x); min_point.y = std::min(min_point.y, p.y); min_point.z = std::min(min_point.z, p.z);
		max_point.x = std::max(max_point.x, p.x); max_point.y = std::max(max_point.y, p.y); max_point.z = std::max(max_point.z, p.z);
	}
	double model_scale = std::max(1.0, (double)GetSize(max_point - min_point));
	const double solve_epsilon = model_scale * 1e-6;
	std::vector<MQPoint> next_offsets(region.Vertices.size(), MQPoint(0, 0, 0));
	for (int iteration = 0; iteration < 300; ++iteration) {
		double max_change = 0.0;
		for (size_t vi = 0; vi < region.Vertices.size(); ++vi) {
			if (fixed[vi] || weighted_adjacency[vi].empty()) {
				next_offsets[vi] = region.Vertices[vi].SolvedTangentOffset;
				continue;
			}

			MQPoint sum(0, 0, 0);
			double weight_sum = 0.0;
			for (size_t ni = 0; ni < weighted_adjacency[vi].size(); ++ni) {
				sum += region.Vertices[weighted_adjacency[vi][ni].first].SolvedTangentOffset * (float)weighted_adjacency[vi][ni].second;
				weight_sum += weighted_adjacency[vi][ni].second;
			}
			if (weight_sum <= 0.0) {
				next_offsets[vi] = region.Vertices[vi].SolvedTangentOffset;
				continue;
			}

			MQPoint average_normal = region.Vertices[vi].AverageNormal;
			if (GetInnerProduct(average_normal, average_normal) <= 1e-12f) {
				average_normal = MQPoint(0, 0, 1);
			}
			average_normal = Normalize(average_normal);

			MQPoint averaged = sum / (float)weight_sum;
			next_offsets[vi] = ProjectVectorToPlane(averaged, average_normal);
			max_change = std::max(max_change, (double)GetSize(next_offsets[vi] - region.Vertices[vi].SolvedTangentOffset));
		}
		for (size_t vi = 0; vi < region.Vertices.size(); ++vi) region.Vertices[vi].SolvedTangentOffset = next_offsets[vi];
		if (max_change <= solve_epsilon) break;
	}

	for (size_t vi = 0; vi < region.Vertices.size(); ++vi) {
		TempVertex& vertex = region.Vertices[vi];
		MQPoint average_normal = vertex.AverageNormal;
		if (GetInnerProduct(average_normal, average_normal) <= 1e-12f) {
			average_normal = MQPoint(0, 0, 1);
		}
		average_normal = Normalize(average_normal);
		vertex.NewPosition = vertex.OriginalPosition + vertex.SolvedTangentOffset + average_normal * (float)depth;
	}

	return true;
}

bool InsetFacePlugin::SolveRegionPlanar(TempRegion& region, double thickness, double depth, bool even_offset)
{
	ProjectRegionVertices(region);
	if (!ExtractBoundaryLoops(region)) {
		return false;
	}
	return SolveRegionInterior2D(region, thickness, depth, even_offset);
}

bool InsetFacePlugin::SolveRegionSurfaceAware(TempRegion& region, double thickness, double depth, bool even_offset)
{
	const double original_thickness = thickness;
	for (int attempt = 0; attempt < 24; ++attempt) {
		const double scale = std::pow(0.5, (double)attempt);
		if (!BuildSurfaceAwareBoundaryTargets(region, original_thickness * scale, even_offset)) continue;
		if (!SolveRegionInterior3D(region, depth)) continue;

		bool valid = true;
		for (size_t fi = 0; fi < region.Faces.size() && valid; ++fi) {
			const TempFace& face = region.Faces[fi];
			std::vector<MQPoint> original(face.Vertices.size()), updated(face.Vertices.size());
			for (size_t vi = 0; vi < face.Vertices.size(); ++vi) {
				const TempVertex& vertex = region.Vertices[face.Vertices[vi]];
				original[vi] = vertex.OriginalPosition;
				updated[vi] = vertex.NewPosition;
				if (!std::isfinite(updated[vi].x) || !std::isfinite(updated[vi].y) || !std::isfinite(updated[vi].z)) valid = false;
			}
			if (!valid || original.size() < 3) continue;
			MQPoint old_normal = ComputeFaceNormalFromPoints(original);
			MQPoint new_normal = ComputeFaceNormalFromPoints(updated);
			const float old_area2 = GetInnerProduct(old_normal, old_normal);
			const float new_area2 = GetInnerProduct(new_normal, new_normal);
			if (old_area2 <= 1e-12f || new_area2 <= old_area2 * 1e-8f || GetInnerProduct(old_normal, new_normal) <= 0.0f) valid = false;
			for (size_t vi = 0; vi < updated.size() && valid; ++vi) {
				const float old_edge_length = GetSize(original[(vi + 1) % original.size()] - original[vi]);
				const float new_edge_length = GetSize(updated[(vi + 1) % updated.size()] - updated[vi]);
				if (old_edge_length <= 1e-8f || new_edge_length <= old_edge_length * 1e-6f) valid = false;
			}
		}
		if (valid) {
			if (attempt > 0) AppendLocalizedWarning(region.Warning, "WarnCollapsedClamped");
			return true;
		}
	}
	AppendLocalizedWarning(region.Warning, "WarnCollapsed");
	return false;
}

bool InsetFacePlugin::BuildTempRegion(MQObject obj, int object_index, const std::vector<FaceInfo>& faces, TempRegion& region)
{
	region = TempRegion();
	region.ObjectIndex = object_index;

	std::set<int> region_face_indices;
	for (size_t fi = 0; fi < faces.size(); ++fi) {
		region_face_indices.insert(faces[fi].FaceIndex);
	}

	std::map<int, int> original_to_temp;
	for (size_t fi = 0; fi < faces.size(); ++fi) {
		TempFace temp_face;
		temp_face.OriginalFaceIndex = faces[fi].FaceIndex;
		temp_face.FaceUniqueID = faces[fi].FaceUniqueID;
		temp_face.MaterialIndex = faces[fi].MaterialIndex;
		temp_face.FaceNormal = faces[fi].FaceNormal;
		temp_face.UV = faces[fi].UV;

		for (size_t vi = 0; vi < faces[fi].Vertices.size(); ++vi) {
			int original_vertex = faces[fi].Vertices[vi];
			std::map<int, int>::iterator found = original_to_temp.find(original_vertex);
			if (found == original_to_temp.end()) {
				TempVertex temp_vertex;
				temp_vertex.OriginalVertexIndex = original_vertex;
				temp_vertex.OriginalPosition = obj->GetVertex(original_vertex);
				temp_vertex.NewPosition = temp_vertex.OriginalPosition;
				temp_vertex.AverageNormal = ComputeVertexNormal(obj, original_vertex, region_face_indices);
				int temp_index = (int)region.Vertices.size();
				region.Vertices.push_back(temp_vertex);
				original_to_temp.insert(std::make_pair(original_vertex, temp_index));
				found = original_to_temp.find(original_vertex);
			}
			temp_face.Vertices.push_back(found->second);
		}

		// Reject structurally degenerate input independently of the chosen offset solver.
		std::set<int> distinct_vertices(temp_face.Vertices.begin(), temp_face.Vertices.end());
		bool valid_face = temp_face.Vertices.size() >= 3 && distinct_vertices.size() == temp_face.Vertices.size();
		std::vector<MQPoint> polygon(temp_face.Vertices.size());
		for (size_t vi = 0; vi < temp_face.Vertices.size(); ++vi) {
			polygon[vi] = region.Vertices[temp_face.Vertices[vi]].OriginalPosition;
			const MQPoint& next = region.Vertices[temp_face.Vertices[(vi + 1) % temp_face.Vertices.size()]].OriginalPosition;
			valid_face = valid_face && std::isfinite(polygon[vi].x) && std::isfinite(polygon[vi].y)
				&& std::isfinite(polygon[vi].z) && GetSize(next - polygon[vi]) > 0.0f;
		}
		const double area = PolygonArea3D(polygon);
		if (!valid_face || !std::isfinite(area) || area <= 0.0) {
			wchar_t detail[256];
			swprintf_s(detail, LocalizedText("WarnInvalidFaceInput", L"Invalid face input (object %d, face %d)").c_str(), object_index + 1, temp_face.OriginalFaceIndex + 1);
			AppendWarning(region.Warning, detail);
			return false;
		}
		region.Faces.push_back(temp_face);
	}

	std::map<std::pair<int, int>, int> directed_edge_map;
	for (size_t fi = 0; fi < region.Faces.size(); ++fi) {
		TempFace& face = region.Faces[fi];
		size_t point_count = face.Vertices.size();
		face.HalfEdges.resize(point_count);
		for (size_t vi = 0; vi < point_count; ++vi) {
			TempHalfEdge halfedge;
			halfedge.StartVertex = face.Vertices[vi];
			halfedge.EndVertex = face.Vertices[(vi + 1) % point_count];
			halfedge.FaceIndex = (int)fi;
			halfedge.FaceVertexIndex = (int)vi;
			face.HalfEdges[vi] = (int)region.HalfEdges.size();
			region.HalfEdges.push_back(halfedge);
		}
		for (size_t vi = 0; vi < point_count; ++vi) {
			TempHalfEdge& halfedge = region.HalfEdges[face.HalfEdges[vi]];
			halfedge.Next = face.HalfEdges[(vi + 1) % point_count];
			halfedge.Prev = face.HalfEdges[(vi + point_count - 1) % point_count];
		}
	}

	for (size_t hi = 0; hi < region.HalfEdges.size(); ++hi) {
		std::pair<int, int> reverse_key(region.HalfEdges[hi].EndVertex, region.HalfEdges[hi].StartVertex);
		std::map<std::pair<int, int>, int>::iterator reverse = directed_edge_map.find(reverse_key);
		if (reverse != directed_edge_map.end()) {
			int other = reverse->second;
			if (region.HalfEdges[other].Pair == -1) {
				region.HalfEdges[hi].Pair = other;
				region.HalfEdges[other].Pair = (int)hi;
			}
			else {
				AppendLocalizedWarning(region.Warning, "WarnNonManifoldSkipped");
			}
		}
		else {
			directed_edge_map.insert(std::make_pair(std::make_pair(region.HalfEdges[hi].StartVertex, region.HalfEdges[hi].EndVertex), (int)hi));
		}
	}

	ClassifyBoundaryElements(region);
	const bool region_mode = m_Params.CurrentMode == InsetParameters::ModeRegion;
	bool nearly_planar = false;
	if (region_mode) {
		if (!FitLocalPlane(region, faces)) return false;
		nearly_planar = IsRegionNearlyPlanar(region, faces, m_Params.Thickness);
	}
	// Region + EvenOffset is solved from the complete boundary. Other paths
	// still use the per-face inset as their local displacement source.
	for (size_t fi = 0; !region_mode && fi < region.Faces.size(); ++fi) {
		if (!BuildFaceLocalInset(region, region.Faces[fi])) {
			AppendLocalizedWarning(region.Warning, "WarnFaceInsetFailed");
			return false;
		}
	}

	if (region_mode) {
		bool solved = false;
		if (nearly_planar) {
			solved = SolveRegionPlanar(region, m_Params.Thickness, m_Params.Depth, m_Params.EvenOffset);
		}
		else {
			solved = SolveRegionSurfaceAware(region, m_Params.Thickness, m_Params.Depth, m_Params.EvenOffset);
		}
		if (!solved) {
			AppendLocalizedWarning(region.Warning, "WarnPatchSolveFailed");
			return false;
		}
	}
	else {
		if (!SolvePatchInsetVertices(region, m_Params.Depth)) {
			AppendLocalizedWarning(region.Warning, "WarnPatchSolveFailed");
			return false;
		}
	}

	region.Valid = true;
	return true;
}

void InsetFacePlugin::AppendPreviewLinesFromRegion(const TempRegion& region, PreviewObject& preview_object) const
{
	if (std::fabs(m_Params.Thickness) <= kGeometryEpsilon) {
		return;
	}

	std::set<EdgeKey> inner_edges;
	for (size_t fi = 0; fi < region.Faces.size(); ++fi) {
		const TempFace& face = region.Faces[fi];
		for (size_t vi = 0; vi < face.Vertices.size(); ++vi) {
			int temp_a = face.Vertices[vi];
			int temp_b = face.Vertices[(vi + 1) % face.Vertices.size()];
			EdgeKey edge(temp_a, temp_b);
			if (inner_edges.insert(edge).second) {
				preview_object.Lines.push_back(PreviewLine{ region.Vertices[temp_a].NewPosition, region.Vertices[temp_b].NewPosition });
			}
		}
	}

	std::set<int> boundary_vertices_drawn;
	for (size_t hi = 0; hi < region.HalfEdges.size(); ++hi) {
		const TempHalfEdge& halfedge = region.HalfEdges[hi];
		if (!halfedge.Boundary) continue;
		if (boundary_vertices_drawn.insert(halfedge.StartVertex).second) {
			preview_object.Lines.push_back(PreviewLine{ region.Vertices[halfedge.StartVertex].OriginalPosition, region.Vertices[halfedge.StartVertex].NewPosition });
		}
		if (boundary_vertices_drawn.insert(halfedge.EndVertex).second) {
			preview_object.Lines.push_back(PreviewLine{ region.Vertices[halfedge.EndVertex].OriginalPosition, region.Vertices[halfedge.EndVertex].NewPosition });
		}
	}
}

bool InsetFacePlugin::BuildPreviewForFaceGroup(MQDocument doc, int object_index, const std::vector<FaceInfo>& faces, PreviewObject& preview_object, RegionCommitData& commit_data)
{
	MQObject obj = doc->GetObject(object_index);
	if (obj == NULL) return false;

	TempRegion region;
	if (!BuildTempRegion(obj, object_index, faces, region)) {
		AppendWarning(m_LastWarning, region.Warning);
		return false;
	}

	AppendWarning(m_LastWarning, region.Warning);
	AppendPreviewLinesFromRegion(region, preview_object);
	commit_data.ObjectIndex = object_index;
	commit_data.Region = region;
	return true;
}

MQColor InsetFacePlugin::GetFaceBaseColor(MQDocument doc, MQObject obj, int face_index)
{
	if (obj != NULL && face_index >= 0) {
		int material_index = obj->GetFaceMaterial(face_index);
		if (material_index >= 0 && material_index < doc->GetMaterialCount()) {
			MQMaterial material = doc->GetMaterial(material_index);
			if (material != NULL) {
				return material->GetColor();
			}
		}
		return obj->GetColor();
	}

	return GetSystemColor(MQSYSTEMCOLOR_OBJECT);
}

MQColor InsetFacePlugin::MixColor(const MQColor& base, const MQColor& highlight, double highlight_weight) const
{
	float weight = (float)std::max(0.0, std::min(1.0, highlight_weight));
	float base_weight = 1.0f - weight;
	MQColor mixed = base * base_weight + highlight * weight;
	mixed.r = std::max(0.0f, std::min(1.0f, mixed.r));
	mixed.g = std::max(0.0f, std::min(1.0f, mixed.g));
	mixed.b = std::max(0.0f, std::min(1.0f, mixed.b));
	return mixed;
}

bool InsetFacePlugin::RebuildPreview(MQDocument doc)
{
	m_Preview.Clear();
	m_PreviewDirty = false;
	m_LastWarning.clear();

	int object_count = doc->GetObjectCount();
	m_Preview.Objects.resize(object_count);
	size_t completed_faces = 0;

	for (int oi = 0; oi < object_count; ++oi) {
		std::vector<FaceInfo> faces;
		if (!BuildFaceInfoList(doc, oi, faces)) return false;
		if (faces.empty()) continue;

		std::vector<std::vector<FaceInfo> > groups;
		BuildFaceGroups(faces, groups);
		for (size_t gi = 0; gi < groups.size(); ++gi) {
			RegionCommitData commit_data;
			if (BuildPreviewForFaceGroup(doc, oi, groups[gi], m_Preview.Objects[oi], commit_data) && commit_data.Region.Valid) {
				m_Preview.CommitData.push_back(commit_data);
				completed_faces += groups[gi].size();
			}
		}
	}

	m_Preview.Valid = true;
	m_Preview.Complete = !m_SelectedFaces.empty() && completed_faces == m_SelectedFaces.size();
	if (!m_SelectedFaces.empty() && !m_Preview.Complete) {
		AppendLocalizedWarning(m_LastWarning, "WarnIncompletePreview");
	}
	SetStatus();
	return m_Preview.Complete;
}

void InsetFacePlugin::OnDraw(MQDocument doc, MQScene scene, int width, int height)
{
	if (!m_Activated) return;

	EnsurePreview(doc);

	if (m_HoverFaceValid &&
		!m_Drag.Pending &&
		!m_Drag.Active) {
		MQObject obj = doc->GetObject(m_HoverObjectIndex);
		if (obj != NULL) {
			int face_index = obj->GetFaceIndexFromUniqueID(m_HoverFaceUniqueID);
			if (face_index >= 0) {
				int point_count = obj->GetFacePointCount(face_index);
				if (point_count >= 3) {
					std::vector<int> face_vertices(point_count);
					obj->GetFacePointArray(face_index, face_vertices.data());
					MQObject draw_hover = CreateDrawingObject(doc, DRAW_OBJECT_FACE);
					draw_hover->AddRenderFlag(MQOBJECT_RENDER_FACE);
					draw_hover->AddRenderFlag(MQOBJECT_RENDER_OVERWRITEFACE);
					draw_hover->AddRenderEraseFlag(MQOBJECT_RENDER_MULTILIGHT);
					draw_hover->AddRenderEraseFlag(MQOBJECT_RENDER_SHADOW);
					int hover_material_index = -1;
					MQMaterial hover_material = CreateDrawingMaterial(doc, hover_material_index);
					MQColor base_color = GetFaceBaseColor(doc, obj, face_index);
					MQColor highlight_color = GetSystemColor(MQSYSTEMCOLOR_HIGHLIGHT);
					hover_material->SetColor(MixColor(base_color, highlight_color, 0.35));
					hover_material->SetDiffuse(0.5f);
					hover_material->SetEmission(0.5f);
					hover_material->SetAmbient(0.0f);
					hover_material->SetSpecular(0.0f);
					std::vector<int> indices(point_count);
					for (int i = 0; i < point_count; ++i) {
						indices[i] = draw_hover->AddVertex(obj->GetVertex(face_vertices[i]));
					}
					int hover_face_index = draw_hover->AddFace(point_count, indices.data());
					if (hover_face_index >= 0) {
						draw_hover->SetFaceMaterial(hover_face_index, hover_material_index);
					}
				}
			}
		}
	}

	for (size_t oi = 0; oi < m_Preview.Objects.size(); ++oi) {
		const PreviewObject& preview = m_Preview.Objects[oi];
		if (preview.Lines.empty()) continue;
		MQObject draw_line = CreateDrawingObject(doc, DRAW_OBJECT_LINE);
		draw_line->AddRenderFlag(MQOBJECT_RENDER_LINE);
		draw_line->AddRenderFlag(MQOBJECT_RENDER_OVERWRITELINE);
		draw_line->AddRenderEraseFlag(MQOBJECT_RENDER_MULTILIGHT);
		draw_line->AddRenderEraseFlag(MQOBJECT_RENDER_SHADOW);
		draw_line->SetColor(GetSystemColor(MQSYSTEMCOLOR_TEMP));
		draw_line->SetColorValid(TRUE);
		for (size_t li = 0; li < preview.Lines.size(); ++li) {
			int indices[2];
			indices[0] = draw_line->AddVertex(preview.Lines[li].A);
			indices[1] = draw_line->AddVertex(preview.Lines[li].B);
			draw_line->AddFace(2, indices);
		}
	}
}

BOOL InsetFacePlugin::OnLeftButtonDown(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	if (!m_Activated) return FALSE;

	m_Drag = DragState();

	HIT_TEST_PARAM hit_param;
	hit_param.TestVertex = FALSE;
	hit_param.TestLine = FALSE;
	hit_param.TestFace = TRUE;
	hit_param.DisableFrontOnly = FALSE;
	hit_param.DisableCoverByFace = FALSE;

	if (HitTest(scene, state.MousePos, hit_param) && hit_param.HitType == HIT_TYPE_FACE) {
		MQObject obj = doc->GetObject(hit_param.ObjectIndex);
		if (obj != NULL) {
			UINT face_unique_id = obj->GetFaceUniqueID(hit_param.FaceIndex);
			m_Drag.Pending = true;
			m_Drag.PreserveSelectionForDrag = IsFaceSelected(hit_param.ObjectIndex, face_unique_id) ? true : false;
			m_Drag.ShiftAtMouseDown = state.Shift;
			m_Drag.HitObjectIndex = hit_param.ObjectIndex;
			m_Drag.HitFaceUniqueID = face_unique_id;
			m_Drag.StartPoint = state.MousePos;
			m_Drag.StartThickness = m_Params.Thickness;
			return TRUE;
		}
	}

	if (!m_SelectedFaces.empty()) {
		m_SelectedFaces.clear();
		SyncDocumentSelection(doc);
		SyncSelectionFromDocument(doc);
		m_LastWarning.clear();
		RedrawAllScene();
	}
	return TRUE;
}

BOOL InsetFacePlugin::OnLeftButtonMove(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	if (!m_Activated) return FALSE;

	if (m_Drag.Pending && !m_Drag.Active) {
		int dx = state.MousePos.x - m_Drag.StartPoint.x;
		int dy = state.MousePos.y - m_Drag.StartPoint.y;
		if (dx * dx + dy * dy >= kDragThresholdPixels * kDragThresholdPixels) {
			m_Drag.Pending = false;
			SelectedFaceRef ref(m_Drag.HitObjectIndex, m_Drag.HitFaceUniqueID);
			if (m_Drag.PreserveSelectionForDrag) {
				// Keep the full multi-selection for Region dragging.
			}
			else if (m_Drag.ShiftAtMouseDown) {
				m_SelectedFaces.insert(ref);
				SyncDocumentSelection(doc, m_Drag.HitObjectIndex);
				SyncSelectionFromDocument(doc);
				m_LastWarning.clear();
			}
			else {
				m_SelectedFaces.clear();
				m_SelectedFaces.insert(ref);
				SyncDocumentSelection(doc, m_Drag.HitObjectIndex);
				SyncSelectionFromDocument(doc);
				m_LastWarning.clear();
			}

			if (IsFaceSelected(m_Drag.HitObjectIndex, m_Drag.HitFaceUniqueID)) {
				m_Drag.Active = true;
				m_Drag.Moved = true;
			}
		}
	}

	if (m_Drag.Active) {
		int dx = state.MousePos.x - m_Drag.StartPoint.x;
		m_Params.Thickness = m_Drag.StartThickness + dx * kThicknessScale;
		if (m_Window) m_Window->SyncFromPlugin();
		InvalidatePreview();
		SetStatus();
		RedrawScene(scene);
		return TRUE;
	}

	return TRUE;
}

BOOL InsetFacePlugin::OnLeftButtonUp(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	if (!m_Activated) return FALSE;

	if (m_Drag.Active) {
		m_Drag.Active = false;
		m_Drag.Pending = false;
		bool applied = ApplyPreview(doc, ApplySource::Drag);
		m_Drag = DragState();
		if (!applied) {
			InvalidatePreview();
			RedrawAllScene();
		}
		return TRUE;
	}

	if (m_Drag.Pending) {
		ResolveClickSelection(doc, m_Drag.HitObjectIndex, m_Drag.HitFaceUniqueID, m_Drag.ShiftAtMouseDown ? true : false);
		m_Drag = DragState();
		RedrawAllScene();
		return TRUE;
	}

	return TRUE;
}

BOOL InsetFacePlugin::OnMouseMove(MQDocument doc, MQScene scene, MOUSE_BUTTON_STATE& state)
{
	if (!m_Activated) return FALSE;

	if (m_Drag.Active || m_Drag.Pending) {
		SetMouseCursor(m_MoveCursor ? m_MoveCursor : GetResourceCursor(MQCURSOR_DEFAULT));
		return FALSE;
	}

	bool redraw = RefreshSelectionFromDocument(doc);

	HIT_TEST_PARAM hit_param;
	hit_param.TestVertex = FALSE;
	hit_param.TestLine = FALSE;
	hit_param.TestFace = TRUE;
	hit_param.DisableFrontOnly = FALSE;
	hit_param.DisableCoverByFace = FALSE;

	bool new_hover_valid = false;
	int new_hover_object = -1;
	UINT new_hover_face = 0;

	if (HitTest(scene, state.MousePos, hit_param) && hit_param.HitType == HIT_TYPE_FACE) {
		MQObject obj = doc->GetObject(hit_param.ObjectIndex);
		if (obj != NULL) {
			new_hover_valid = true;
			new_hover_object = hit_param.ObjectIndex;
			new_hover_face = obj->GetFaceUniqueID(hit_param.FaceIndex);
		}
	}

	if (new_hover_valid != m_HoverFaceValid || new_hover_object != m_HoverObjectIndex || new_hover_face != m_HoverFaceUniqueID) {
		m_HoverFaceValid = new_hover_valid;
		m_HoverObjectIndex = new_hover_object;
		m_HoverFaceUniqueID = new_hover_face;
		redraw = true;
	}

	if (m_HoverFaceValid) {
		SetMouseCursor(m_MoveCursor ? m_MoveCursor : GetResourceCursor(MQCURSOR_DEFAULT));
	}
	else {
		SetMouseCursor(GetResourceCursor(MQCURSOR_DEFAULT));
	}

	if (redraw) {
		RedrawScene(scene);
	}
	return FALSE;
}

BOOL InsetFacePlugin::OnKeyDown(MQDocument doc, MQScene scene, int key, MOUSE_BUTTON_STATE& state)
{
	if (!m_Activated) return FALSE;

	if (key == VK_RETURN) {
		if (ApplyPreview(doc, ApplySource::Keyboard)) {
			RedrawAllScene();
		}
		return TRUE;
	}
	if (key == VK_ESCAPE) {
		CancelCurrentOperation();
		return TRUE;
	}
	return FALSE;
}

bool InsetFacePlugin::ApplyCommitDataToObject(MQDocument doc, MQObject obj, const RegionCommitData& commit_data)
{
	const TempRegion& region = commit_data.Region;
	if (!region.Valid) return false;

	obj->ReserveVertex(obj->GetVertexCount() + (int)region.Vertices.size());
	obj->ReserveFace(obj->GetFaceCount() + (int)region.Faces.size() + (int)region.HalfEdges.size());

	std::vector<int> duplicated_vertices(region.Vertices.size(), -1);
	for (size_t vi = 0; vi < region.Vertices.size(); ++vi) {
		duplicated_vertices[vi] = obj->AddVertex(region.Vertices[vi].NewPosition);
		if (duplicated_vertices[vi] == -1) throw std::bad_alloc();
	}

	for (size_t fi = 0; fi < region.Faces.size(); ++fi) {
		std::vector<int> face_vertices(region.Faces[fi].Vertices.size());
		for (size_t vi = 0; vi < region.Faces[fi].Vertices.size(); ++vi) {
			face_vertices[vi] = duplicated_vertices[region.Faces[fi].Vertices[vi]];
		}
		int face_index = obj->AddFace((int)face_vertices.size(), face_vertices.data());
		if (face_index == -1) throw std::bad_alloc();
		obj->SetFaceMaterial(face_index, region.Faces[fi].MaterialIndex);
		obj->SetFaceCoordinateArray(face_index, const_cast<MQCoordinate*>(region.Faces[fi].UV.data()));
	}

	for (size_t hi = 0; hi < region.HalfEdges.size(); ++hi) {
		const TempHalfEdge& halfedge = region.HalfEdges[hi];
		if (!halfedge.Boundary) continue;

		const TempFace& source_face = region.Faces[halfedge.FaceIndex];
		int quad[4];
		quad[0] = region.Vertices[halfedge.StartVertex].OriginalVertexIndex;
		quad[1] = region.Vertices[halfedge.EndVertex].OriginalVertexIndex;
		quad[2] = duplicated_vertices[halfedge.EndVertex];
		quad[3] = duplicated_vertices[halfedge.StartVertex];
		int side_face = obj->AddFace(4, quad);
		if (side_face == -1) throw std::bad_alloc();

		MQCoordinate quad_uv[4];
		quad_uv[0] = source_face.UV[halfedge.FaceVertexIndex];
		quad_uv[1] = source_face.UV[(halfedge.FaceVertexIndex + 1) % source_face.UV.size()];
		quad_uv[2] = source_face.UV[(halfedge.FaceVertexIndex + 1) % source_face.UV.size()];
		quad_uv[3] = source_face.UV[halfedge.FaceVertexIndex];
		obj->SetFaceMaterial(side_face, source_face.MaterialIndex);
		obj->SetFaceCoordinateArray(side_face, quad_uv);
	}

	for (size_t fi = 0; fi < region.Faces.size(); ++fi) {
		int face_index = obj->GetFaceIndexFromUniqueID(region.Faces[fi].FaceUniqueID);
		if (face_index >= 0) {
			obj->DeleteFace(face_index, false);
		}
	}

	obj->Compact();
	return true;
}

void InsetFacePlugin::ResetAfterApply(MQDocument doc, ApplySource source)
{
	m_SelectedFaces.clear();
	ClearDocumentSelection(doc);
	if (source == ApplySource::Drag) {
		m_Params.Thickness = 0.0;
		m_Params.Depth = 0.0;
	}
	ResetToolState(false);
	if (m_Window) m_Window->SyncFromPlugin();
	SetStatus();
}

bool InsetFacePlugin::ApplyPreview(MQDocument doc, ApplySource source)
{
	LoadResource();
	if (m_SelectedFaces.empty()) {
		const std::wstring text = LocalizedText("StatusNoSelectedFaces", L"Inset Face: no selected faces.");
		SetStatusString(text.c_str());
		return false;
	}
	if (!EnsurePreview(doc)) {
		// Preserve both the selection and the detailed failure diagnostics.
		SetStatus();
		return false;
	}

	bool modified = false;
	for (size_t i = 0; i < m_Preview.CommitData.size(); ++i) {
		const RegionCommitData& commit_data = m_Preview.CommitData[i];
		MQObject obj = doc->GetObject(commit_data.ObjectIndex);
		if (obj == NULL) continue;
		if (ApplyCommitDataToObject(doc, obj, commit_data)) {
			modified = true;
			obj->UpdateNormal();
		}
	}

	if (modified) {
		const std::wstring undo_label = LocalizedText("UndoLabel", L"Inset Face");
		UpdateUndo(undo_label.c_str());
		ResetAfterApply(doc, source);
	}
	else {
		InvalidatePreview();
		SetStatus();
	}

	RedrawAllScene();
	return modified;
}

MQBasePlugin* GetPluginClass()
{
	static InsetFacePlugin plugin;
	return &plugin;
}
