WRITEN IN ENGLISH

# Build System
In general, using CMake(4.0.0+) as the first choice, Bazel(9.3.3+) and Meson(1.12.1+) as the secord.
## CMake
- Support PGO optimization for MSVC, Clang, GCC.
- Use system deps first and then use CPM to get deps. While using MSVC workflows, enforce using CPM for ABI compatibility.
- 


# Code Style
## File
- Using snake_case

## Code
- Use PascalCase for class, struct.
- Use snake_case for variable, function signature.
- Use CONSTANT_CASE for constants.
- Use class instead of typename in template.
- Use trailing return type in function signature.

# Deps
- Python 3.14+
- CMake 4.0.0+
- google/benchmark 1.10.0+
- gtest v1.18.0+
- absl 20260817.0+

# CI/CD