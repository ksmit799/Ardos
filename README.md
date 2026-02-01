# Ardos

> A Realtime Distributed Object Server

*Based on [Astron](https://github.com/astron/Astron) (Modified BSD 3-Clause)*

Ardos is a highly-distributed networking suite designed to power large-scale MMO games.
Inspired by the in-house OTP (Online Theme Park) networking technology developed by Disney Interactive Studios, which
was used to power MMOs such as Toontown Online, Pirates of The Caribbean Online, Pixie Hollow Online, and The World of
Cars Online.

Ardos is first and foremost designed to be a first class citizen of the [Panda3D](https://www.panda3d.org/) game
engine (developed by Disney and CMU), which has built-in, production ready support for this server technology.
However, through tools such as [otp-gen](https://github.com/ksmit799/otp-gen), it's possible to use Ardos via other
game engines/languages.

It's highly recommended to view the Ardos [wiki](https://github.com/ksmit799/Ardos/wiki) for build instructions,
configuration examples, deployment strategies, networking topology, and help on determining whether Ardos is the right
fit for you.

## Building

Ardos uses CMake and vcpkg for dependency management. vcpkg is included as a git submodule, so you do not need to install it separately.

### Prerequisites

- CMake 3.22 or later
- C++20 compatible compiler

### Building Ardos

1. Clone the repository and its submodules (this includes vcpkg):
   ```bash
   git clone --recurse-submodules https://github.com/ksmit799/Ardos.git
   cd Ardos
   ```
   If you already cloned without submodules:
   ```bash
   git submodule update --init --recursive
   ```

2. Configure CMake using the bundled vcpkg toolchain:
   ```bash
   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=libs/vcpkg/scripts/buildsystems/vcpkg.cmake
   ```

3. Build:
   ```bash
   cmake --build build
   ```

### Optional: Database Server Support

To build with MongoDB database server support, ensure `ARDOS_WANT_DB_SERVER` is enabled (it's ON by default). The MongoDB C++ driver will be automatically installed via vcpkg.

### Optional: Legacy Mode

Ardos supports building in "legacy" mode, which makes the cluster compatible with original Disney clients. Generally, this shouldn't be used for new projects. To enable legacy mode, compile with `ARDOS_USE_LEGACY_CLIENT`. 

## OTP Architecture Resources

> Helpful for understanding the internal architecture of Ardos and its usages.

### Video Lectures (Disney Interactive Studios)

- [DistributedObjects](http://www.youtube.com/watch?v=JsgCFVpXQtQ)
- [DistributedObjects and the OTP Server](http://www.youtube.com/watch?v=r_ZP9SInPcs)
- [OTP Server Internals](http://www.youtube.com/watch?v=SzybRdxjYoA)
- [(GDC Online) MMO 101 - Building Disney's Server System](https://www.gdcvault.com/play/1013776/MMO-101-Building-Disney-s)

### Presentation Slides

- [MMO 101 - Building Disney's Server](http://twvideo01.ubm-us.net/o1/vault/gdconline10/slides/11516-MMO_101_Building_Disneys_Sever.pdf)

### Other Documentation

- [Building a MMG for the Millions - Disney's Toontown](http://dl.acm.org/citation.cfm?id=950566.950589)
