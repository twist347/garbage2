from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain
from conan.tools.files import copy
from conan.tools.scm import Git

class EDA(ConanFile):
    name = "eda"
    settings = "os", "compiler", "build_type", "arch"
    default_options = {
    }
    generators = "CMakeDeps"
    layouts = "cmake_layout"

    # todo : migrate to version config as below instead of approach from ci-cd/scripts/15.22/deploy.sh
    # def set_version(self):
    #     git = Git(self)
    #     version = git.run("describe --tags --abbrev=8 --always")
    #     self.output.info("Delivering application version: {}".format(version))
    #     self.version = version

    def configure(self):
        if self.settings.os == 'Windows':
            self.options["boost/*"].fPIC = True
            self.options["boost/*"].extra_b2_flags = "define=_WIN32_WINNT=0x0A00"

    # Example of conditional options setup 
    # def config_options(self):
    #     if self.settings.os == 'Windows':
    #         self.options["qt/*"].with_freetype = False
    #     else:
    #         self.options["qt/*"].with_freetype = True

    def requirements(self):
        # Notes:
        # 1. Please keep the alphabet order in requires!
        # 2. libpq, resource_pool requirements are explicit due to OZO missing Windows Conan recipe, thus no way to auto-require those as deps

        # Required by boost and openssl
        self.requires("bzip2/1.0.8@#411fc05e80d47a89045edc1ee6f23c1d")
        self.requires("zlib/1.3.1@#b8bc2603263cf7eccbd6e17e66b0ed76") 
        self.requires("b2/5.3.3@#107c15377719889654eb9a162a673975")
        if self.settings.os == 'Windows':
            self.requires("strawberryperl/5.32.1.1@#8f83d05a60363a422f9033e52d106b47")
        self.requires("nasm/2.16.01@#31e26f2ee3c4346ecd347911bd126904")

        # Required by xlnt
        self.requires("utfcpp/3.2.3@#b374e1027402f3fe561e8d41c0fa6c17")
        self.requires("miniz/3.0.2@#bfbce07c6654293cce27ee24129d2df7")
        self.requires("libstudxml/1.1.0-b.10+1@#dbf5dbaa380ebbf310d689f39812bd73")
        self.requires("expat/2.7.1@#b0b67ba910c5147271b444139ca06953")

        self.requires("boost/1.83.0@#5bcb2a14a35875e328bf312e080d3562")
        self.requires("libpq/15.4@#7c113f89180dee415d80d6b5aa9bab7f")
        self.requires("openssl/3.3.2@#90b3fc29e196eb631636c25d3516cd93")
        self.requires("xlnt/1.5.0@#e5ec04252980531d13216a6a8d295a85")

        # if self.settings.os == 'Windows':
        #     self.output.warning("OZO package configuration is made thru 3rdparty\ozo\windows CMake Find-workaround")
        # else:
        #     self.requires("yandex-ozo/cci.20210509")
        #     # https://github.com/conan-io/conan-center-index/blob/1c4fc7d6fa6af1e238c9f4a783d5c62fe7601108/recipes/yandex-ozo/all/conanfile.py#L62

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["CONAN_BUILD"] = "1" # todo : get rid of this after implementing Conan build for Windows
        if self.settings.os == 'Windows':
            # Check the link for _WIN32_WINNT possible variants: https://learn.microsoft.com/ru-ru/cpp/porting/modifying-winver-and-win32-winnt?view=msvc-170
            tc.preprocessor_definitions["_WIN32_WINNT"] = "0x0A00" # 0x0A00 for win10
            # tc.preprocessor_definitions["BOOST_LOG_DYN_LINK"] = "1" # wtf?
            tc.preprocessor_definitions["LLVM_USE_LINKER"] = "lld"
        tc.generate()

    def build(self):
        # Note: use os check (e.g. "self.settings.os == 'Windows'") for OS-dependant builds

        cmake = CMake(self)
        cmake.configure()

        # Build jobs amount is configured via conan profile. For example take a look at: .conan2/profiles/debian-gcc-debug.example
        cmake.build(
            target = "REL_WEB_CLIENT"
            )
        cmake.build()

        cmake.install()
        # cmake.test()
