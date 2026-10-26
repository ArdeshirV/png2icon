# png2icon

Convert a PNG or JPEG image into a Windows `.ico` file containing common icon sizes (16–256 px). This is useful for Windows applications and shortcuts that need one icon file to look sharp at different display sizes.

## Build

```sh
make                 # Build for the current platform
make windows         # Cross-compile a 64-bit Windows executable with MinGW
```

Linux/Termux builds require a C++17 compiler, Make, libpng, and libjpeg. On Termux:

```sh
pkg install clang make pkg-config libpng libjpeg-turbo
```

Windows builds use the system Windows Imaging Component (WIC); cross-compiling from Linux requires `x86_64-w64-mingw32-g++`.

## Use

```sh
./png2icon image.png       # Creates image.ico
./png2icon photo.jpg app.ico
```

On Windows, run `png2icon.exe` with the same arguments. Use `make clean` to remove the built executable(s).
