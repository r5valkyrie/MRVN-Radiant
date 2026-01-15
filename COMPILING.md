# Compiling MRVN-Radiant

This guide covers how to build MRVN-Radiant from source on Windows and Linux.

---

## Windows

### Option 1: Visual Studio (Recommended)

**Prerequisites:**
- [Visual Studio](https://visualstudio.microsoft.com/) with C++ workload installed
- [Git](https://git-scm.com/)

**Steps:**

1. Clone the repository:
   ```sh
   git clone https://github.com/MRVN-Radiant/MRVN-Radiant.git
   cd MRVN-Radiant
   ```

2. Download dependencies from [MRVN-WinDeps](https://github.com/MRVN-Radiant/MRVN-WinDeps) (latest release):
   - Extract `windeps.7z` into the repository root (so `windeps/` is next to `CMakeLists.txt`)
   - Extract **one** of the following into the `install/` folder:
     - `install.7z` — for Release builds
     - `install-debug.7z` — for Debug builds

3. Open the repository folder in Visual Studio

4. Generate the CMake cache:
   - Open the root `CMakeLists.txt`
   - Click **Generate** on the popup that appears

5. Build the project:
   - Go to **Build → Build All**

---

### Option 2: Visual Studio Command Line

If you prefer using the command line instead of the Visual Studio IDE:

1. Complete steps 1-2 from **Option 1** above

2. Open **"Developer Command Prompt for VS"** or **"Native Tools Command Prompt"**

3. Navigate to the repository and build:
   ```sh
   cd path\to\MRVN-Radiant
   cmake . -G "Ninja"
   cmake --build .
   ```

---

### Option 3: MinGW (GCC)

**Prerequisites:**

1. Install [MSYS2](https://www.msys2.org/) and follow their installation guide

2. Open the **MinGW 64-bit** shell and install dependencies:
   ```sh
   pacman -S make cmake gcc pkg-config unzip base-devel
   pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-openjpeg mingw-w64-x86_64-qt5-base mingw-w64-x86_64-glib2 mingw-w64-x86_64-libxml2 mingw-w64-x86_64-libpng mingw-w64-x86_64-zlib
   ```
   > **Note:** For 32-bit builds, replace `mingw-w64-x86_64` with `mingw-w64-i686`

**Steps:**

1. Navigate to the repository:
   ```sh
   cd /path/to/MRVN-Radiant
   ```

2. Generate and build:
   ```sh
   cmake . -G "MinGW Makefiles" -DINSTALL_DLLS=OFF
   cmake --build .
   ```

**Troubleshooting:**

- **To run Radiant from File Explorer:** You need to copy some DLLs. Run this once:
  ```sh
  cmake . -G "MinGW Makefiles" -DINSTALL_DLLS=ON
  cmake --build .
  ```

- **White screen on launch:** Delete `OPENGL32.dll` from the `install/` folder (or remove Mesa entirely)

---

## Linux

**Prerequisites:**

Install the following dependencies using your package manager:
- qt5
- glib
- libxml2
- zlib
- libpng
- libjpeg

Example for Debian/Ubuntu:
```sh
sudo apt install qtbase5-dev libglib2.0-dev libxml2-dev zlib1g-dev libpng-dev libjpeg-dev
```

**Steps:**

1. Navigate to the repository:
   ```sh
   cd /path/to/MRVN-Radiant
   ```

2. Generate and build:
   ```sh
   cmake . -G "Unix Makefiles"
   cmake --build .
   ```

---

## CMake Options

You can customize the build by adding flags when running `cmake`:

```sh
cmake . -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DBUILD_PLUGINS=OFF
```

| Option | Values | Description |
|--------|--------|-------------|
| `CMAKE_BUILD_TYPE` | `Release` / `Debug` | Build type (optimized vs debug symbols) |
| `BUILD_PLUGINS` | `ON` / `OFF` | Build plugin modules |
| `BUILD_RADIANT` | `ON` / `OFF` | Build the main Radiant editor |
| `BUILD_TOOLS` | `ON` / `OFF` | Build command-line tools |
| `CMAKE_VERBOSE_MAKEFILE` | `ON` / `OFF` | Show detailed build output |