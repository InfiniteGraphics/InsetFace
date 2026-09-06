"""Run with Python in an MSVC developer shell; extracts the current production methods."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'plugin/InsetFace.cpp').read_text(encoding='utf-8-sig')


def extract(signature, struct=False):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + (';' if struct else '')


prefix = r'''
#include <set>
#include <vector>
#include <string>
#include <cassert>
#include <iostream>
using UINT = unsigned int;
'''
prefix += extract('struct SelectedFaceRef', True)
prefix += r'''
struct PreviewObject {};
struct RegionCommitData { int ObjectIndex = 0; struct { bool Valid = false; } Region; };
'''
prefix += extract('struct PreviewMesh', True)
prefix += r'''
struct FakeObject {
 int GetFaceCount() { return 3; }
 int GetFacePointCount(int) { return 4; }
 UINT GetFaceUniqueID(int i) { return i + 1; }
 int GetFaceIndexFromUniqueID(UINT id) { return int(id) - 1; }
 void UpdateNormal() {}
};
using MQObject = FakeObject*;
struct FakeDoc {
 FakeObject object;
 std::set<int> selected;
 int GetObjectCount() { return 1; }
 MQObject GetObject(int i) { return i == 0 ? &object : nullptr; }
 bool IsSelectFace(int, int i) { return selected.count(i) != 0; }
 void DeleteSelectFace(int, int i) { selected.erase(i); }
 void AddSelectFace(int, int i) { selected.insert(i); }
 void SetCurrentObjectIndex(int) {}
};
using MQDocument = FakeDoc*;
struct FaceInfo { UINT id; };
void AppendLocalizedWarning(std::wstring& text, const char*) { text += L"incomplete"; }
std::wstring LocalizedText(const char*, const wchar_t* text) { return text; }
struct InsetFacePlugin {
 enum class ApplySource { Button, Drag, Keyboard };
 std::set<SelectedFaceRef> m_SelectedFaces;
 PreviewMesh m_Preview;
 bool m_PreviewDirty = true;
 std::wstring m_LastWarning;
 std::set<UINT> failed;
 int build_calls = 0, commit_calls = 0, resets = 0;
 void InvalidatePreview();
 bool RefreshSelectionFromDocument(MQDocument);
 void SyncSelectionFromDocument(MQDocument);
 void ClearDocumentSelection(MQDocument);
 void SyncDocumentSelection(MQDocument, int = -1);
 void ResolveClickSelection(MQDocument, int, UINT, bool);
 bool EnsurePreview(MQDocument);
 bool RebuildPreview(MQDocument);
 bool ApplyPreview(MQDocument, ApplySource = ApplySource::Button);
 void SetStatus() {}
 void SetStatusString(const wchar_t*) {}
 void LoadResource() {}
 void UpdateUndo(const wchar_t*) {}
 void RedrawAllScene() {}
 void ResetAfterApply(MQDocument, ApplySource) { ++resets; }
 bool ApplyCommitDataToObject(MQDocument, MQObject, const RegionCommitData&) { ++commit_calls; return true; }
 bool BuildFaceInfoList(MQDocument, int, std::vector<FaceInfo>& faces) {
   ++build_calls;
   for (const auto& ref : m_SelectedFaces) faces.push_back({ref.FaceUniqueID});
   return true;
 }
 void BuildFaceGroups(const std::vector<FaceInfo>& faces, std::vector<std::vector<FaceInfo>>& groups) {
   for (auto face : faces) groups.push_back({face});
 }
 bool BuildPreviewForFaceGroup(MQDocument, int, const std::vector<FaceInfo>& faces, PreviewObject&, RegionCommitData& data) {
   if (failed.count(faces[0].id)) { m_LastWarning += L"bad face;"; return false; }
   data.Region.Valid = true; return true;
 }
};
'''
methods = [
    'void InsetFacePlugin::InvalidatePreview()',
    'bool InsetFacePlugin::RefreshSelectionFromDocument(',
    'void InsetFacePlugin::SyncSelectionFromDocument(',
    'void InsetFacePlugin::ClearDocumentSelection(',
    'void InsetFacePlugin::SyncDocumentSelection(',
    'void InsetFacePlugin::ResolveClickSelection(',
    'bool InsetFacePlugin::EnsurePreview(',
    'bool InsetFacePlugin::RebuildPreview(',
    'bool InsetFacePlugin::ApplyPreview(',
]
main = r'''
int main() {
 FakeDoc doc;
 InsetFacePlugin p;
 p.ResolveClickSelection(&doc, 0, 1, false);
 assert(p.EnsurePreview(&doc));
 p.ResolveClickSelection(&doc, 0, 2, false);
 assert(p.m_PreviewDirty && !p.m_Preview.Valid && p.m_Preview.CommitData.empty());
 assert(p.m_SelectedFaces.size() == 1 && doc.selected == std::set<int>{1});
 assert(p.EnsurePreview(&doc));
 p.ResolveClickSelection(&doc, 0, 3, true);
 assert(p.m_PreviewDirty && p.m_SelectedFaces.size() == 2);
 assert((doc.selected == std::set<int>{1, 2}));
 assert(p.EnsurePreview(&doc));
 p.ResolveClickSelection(&doc, 0, 2, true);
 assert(p.m_PreviewDirty && p.m_SelectedFaces.size() == 1);
 assert(doc.selected == std::set<int>{2});

 for (int failures = 0; failures <= 2; ++failures) {
   InsetFacePlugin q;
   q.ResolveClickSelection(&doc, 0, 1, false);
   q.ResolveClickSelection(&doc, 0, 2, true);
   for (int i = 1; i <= failures; ++i) q.failed.insert(i);
   assert(q.EnsurePreview(&doc) == (failures == 0));
   assert(q.m_Preview.Valid && !q.m_PreviewDirty);
   assert(q.m_Preview.Complete == (failures == 0));
   assert(q.m_Preview.CommitData.size() == size_t(2 - failures));
   int builds = q.build_calls;
   auto selection = q.m_SelectedFaces;
   auto warning = q.m_LastWarning;
   for (int repeat = 0; repeat < 3; ++repeat)
     assert(q.EnsurePreview(&doc) == (failures == 0));
   assert(q.build_calls == builds);
   assert(q.ApplyPreview(&doc) == (failures == 0));
   if (failures) {
     assert(q.commit_calls == 0 && q.resets == 0);
     assert(q.m_SelectedFaces == selection && q.m_LastWarning == warning);
     assert(!warning.empty());
   } else assert(q.commit_calls == 2 && q.resets == 1);
 }
 std::cout << "PASS: click and Shift selection invalidate cache; complete/incomplete preview and atomic apply\n";
}
'''


def run():
    with tempfile.TemporaryDirectory(prefix='inset-preview-test-') as directory:
        path = Path(directory)
        cpp = path / 'preview_state.cpp'
        cpp.write_text(prefix + '\n'.join(extract(m) for m in methods) + main, encoding='utf-8')
        args = ['cl', '/nologo', '/EHsc', '/std:c++14', str(cpp), '/Fe:' + str(path / 'test.exe')]
        if shutil.which('cl'):
            subprocess.run(args, cwd=path, check=True)
        else:
            vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
            install = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
            if not install:
                raise RuntimeError('Install MSVC C++ tools or run from an MSVC developer shell.')
            vcvars = Path(install) / 'VC/Auxiliary/Build/vcvars64.bat'
            command = f'call "{vcvars}" >nul && ' + subprocess.list2cmdline(args)
            subprocess.run(command, shell=True, cwd=path, check=True)
        subprocess.run([str(path / 'test.exe')], check=True)


if __name__ == '__main__':
    run()
