<!--
This file is shown in preference to the root README.md on the GitHub repo homepage
(GitHub's README lookup checks .github/README.md first). Keep it in sync with ../README.md.
See 'release/text/readme.html' for the end user read-me.
-->

> [!NOTE]
> This repository tracks some files with Git LFS. If your initial clone hits LFS-related
> errors, retry with `GIT_LFS_SKIP_SMUDGE=1` set for the clone.

Applender
=========

Applender is a fork of [Blender](https://www.blender.org), the free and open source 3D creation
suite, rebuilt exclusively for **Apple Silicon macOS**. It supports the entirety of the 3D
pipeline—modeling, rigging, animation, simulation, rendering, compositing, motion tracking and
video editing—on top of a **Metal-only** graphics and compute backend.

Applender is not officially affiliated with, endorsed by, or sponsored by the Blender Foundation.
"Blender" is a registered trademark of the Blender Foundation.

![Applender](../doc/images/readme-hero.png "Applender — a fork of Blender for Apple Silicon")

What's different from Blender
------------------------------

- **Apple Silicon (arm64) macOS only.** Windows, Linux, and Intel-Mac code paths, along with the
  OpenGL, Vulkan, CUDA, HIP, and oneAPI backends, have been removed rather than left dormant.
- **Metal-only rendering and compute**, including Cycles Metal ray tracing (opt-in), native NEON
  vectorization, and Apple GPU-specific tuning (occupancy, half-precision shaders, async PSO
  compilation, opt-in on-disk pipeline caching).
- **Hardened by default** (`WITH_HARDENING`): stack protector, libc++ hardening, and FORTIFY.
- **Native MCP support** (`WITH_MCP`): a C++ facade plus embedded-Python transport, exposed to
  Python as `bpy.mcp`.
- User preferences and data continue to live under the `Blender` folder name, so existing
  Blender configurations and add-ons carry over unchanged.

Applender selectively ports upstream Blender's logic, math, and security fixes rather than
tracking every upstream commit; it is not a drop-in replacement for upstream Blender and does not
aim for build or platform parity with it.

Project Pages
-------------

- [Applender Repository](https://github.com/claudy-sky/Applender)
- [Blender Manual](https://docs.blender.org/manual/en/latest/index.html) (applies to shared
  functionality; Applender-specific behavior may differ)
- [Blender Main Website](https://www.blender.org)

Development
-----------

- Build requirements: macOS on Apple Silicon (arm64), Xcode/Metal toolchain. Upstream Blender's
  [Build Instructions](https://developer.blender.org/docs/handbook/building_blender/) apply to
  the shared parts of the build system; platform-specific steps for other operating systems do
  not apply to this fork.
- Issues and pull requests are tracked on the
  [Applender GitHub repository](https://github.com/claudy-sky/Applender).
- For general Blender development background, see the
  [Developer Documentation](https://developer.blender.org/docs/) and
  [Developer Forum](https://devtalk.blender.org).

License
-------

Blender (and thus Applender) as a whole is licensed under the GNU General Public License,
Version 3. Individual files may have a different but compatible license.

See [blender.org/about/license](https://www.blender.org/about/license) for details.
