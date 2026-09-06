# Regression tests

Run from the repository root in an x64 Visual Studio Developer Command Prompt:

```bat
python tests/geometry_regression.py
python tests/run_region_solver_tests.py
python tests/preview_state_regression.py
python tests/region_surface_structure_regression.py
```

Requires Python 3 and MSVC. The scripts extract the current C++ implementation
and compile it with minimal host stubs. They cover offset collapse and boundary
collisions, interior displacement, selection invalidation, and failed previews.
They do not replace interactive validation in Metasequoia.
