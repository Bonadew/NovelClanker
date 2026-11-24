from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class NovelWriterConan(ConanFile):
    name = "novelwriter"
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

    def layout(self):
        cmake_layout(self)

    def generate(self):
        CMakeDeps(self).generate()
        tc = CMakeToolchain(self)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
