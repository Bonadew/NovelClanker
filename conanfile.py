import os
import shutil

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy


class NovelClankerConan(ConanFile):
    name = "novelclanker"
    license = "GPL-3.0-or-later"
    version = "0.1.0"
    settings = "os", "compiler", "build_type", "arch"

    # Only qtbase (Core, Gui, Widgets) is needed. All extra Qt modules are off by default
    # in the recipe; here we also strip qtbase features we don't use to keep the build small.
    default_options = {
        "qt/*:shared": True,
        "qt/*:gui": True,
        "qt/*:widgets": True,
        "qt/*:openssl": False,
        "qt/*:with_icu": False,
        "qt/*:with_sqlite3": False,
        "qt/*:with_pq": False,
        "qt/*:with_odbc": False,
        "qt/*:with_mysql": False,
        "qt/*:with_openal": False,
        "qt/*:with_gstreamer": False,
        "qt/*:with_pulseaudio": False,
        "qt/*:with_libalsa": False,
        "qt/*:with_gssapi": False,
        "qt/*:with_brotli": False,
        "qt/*:with_zstd": False,
        "qt/*:with_libjpeg": False,
        # harfbuzz's glib integration drags in glib -> gettext -> libiconv (autotools builds that are
        # slow and break on Windows paths with spaces). Qt doesn't need it.
        "harfbuzz/*:with_glib": False,
        # Novel storage. LZ4 gives cheap compression for prose.
        "rocksdb/*:with_lz4": True,
    }

    def configure(self):
        if self.settings.os in ("Windows", "Macos"):
            # Qt uses the native font engines here (DirectWrite / CoreText) and its bundled harfbuzz,
            # so skip the external freetype/harfbuzz packages (and their meson builds).
            self.options["qt"].with_freetype = False
            self.options["qt"].with_harfbuzz = False
            self.options["qt"].with_fontconfig = False

    def requirements(self):
        self.requires("qt/6.8.5")
        self.requires("rocksdb/11.1.1")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        CMakeDeps(self).generate()
        tc = CMakeToolchain(self)
        tc.generate()
        # The license files of every library that ends up in the app (Qt and everything built into it or into
        # RocksDB), for "cmake --install" to ship in dist/licenses/<library>/. Build tools are not included.
        # Libraries built into Qt's shared libraries are only listed with tools.graph:skip_binaries=False
        # (set in profiles/windows-msvc and by build.sh); otherwise Conan skips them.
        licenses = os.path.join(self.generators_folder, "licenses")
        shutil.rmtree(licenses, ignore_errors=True)  # no leftovers from dependencies that were dropped
        bundled = []
        # (dependencies.host would leave out the libraries inside Qt: they aren't linked by the app itself.)
        for req, dep in self.dependencies.items():
            if req.build or str(dep.ref.version) == "system":  # build tools; libraries of the operating system
                continue
            if not dep.package_folder:
                self.output.warning(f"No license files for {dep.ref}: its package was skipped. "
                                    "Use -c tools.graph:skip_binaries=False to include them.")
                continue
            copy(self, "*", src=os.path.join(dep.package_folder, "licenses"),
                 dst=os.path.join(licenses, dep.ref.name))
            bundled.append(dep)
        self._write_notices(os.path.join(self.generators_folder, "THIRD-PARTY-NOTICES.txt"), bundled)

    def _write_notices(self, path, deps):
        """THIRD-PARTY-NOTICES.txt for dist/: which libraries are included, under which licenses, from where."""
        def license_text(dep):
            value = dep.license
            return " or ".join(value) if isinstance(value, (list, tuple)) else str(value)

        rows = sorted(deps, key=lambda d: d.ref.name)
        width = max(len(f"{d.ref.name} {d.ref.version}") for d in rows)
        lines = [
            "NovelClanker - third-party software",
            "",
            "NovelClanker is free software under the GNU General Public License, version 3 or (at your option) any",
            "later version; see LICENSE.txt. This copy also contains the libraries below. The license texts of each",
            "are in licenses/<name>/.",
            "",
            "Qt is used under the GNU Lesser General Public License v3. Its libraries are separate shared libraries",
            "(Qt6*.dll on Windows, lib/libQt6*.so* on Linux) that you may replace with your own build of the same Qt",
            f"version ({self.dependencies['qt'].ref.version}). Qt's source code is available from:",
            "https://download.qt.io/archive/qt/",
            "The other libraries are built into NovelClanker or into Qt's libraries.",
            "",
        ]
        for dep in rows:
            name = f"{dep.ref.name} {dep.ref.version}".ljust(width)
            lines.append(f"{name}  {license_text(dep)}")
            if dep.homepage:
                lines.append(f"{''.ljust(width)}  {dep.homepage}")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
