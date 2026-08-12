add_rules("mode.debug", "mode.release")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

package("playback-ffmpeg")
    set_kind("binary")
    set_base("ffmpeg")
    add_urls("https://ffmpeg.org/releases/ffmpeg-$(version).tar.bz2")
    add_versions("7.1", "fd59e6160476095082e94150ada5a6032d7dcc282fe38ce682a00c18e7820528")
    on_load(function (package)
        package:add("deps", "nasm")
        if package:config("libx264") then
            package:add("deps", "x264")
        end
        package:add("deps", "msys2", {configs = {msystem = "MINGW64", base_devel = true}})
    end)
    on_install("@windows", function (package)
        local msys2      = assert(package:dep("msys2"), "MSYS2 dependency was not resolved")
        local msys2_base = assert(msys2:dep("msys2-base"), "MSYS2 base dependency was not resolved")
        local bash        = path.join(msys2_base:installdir("usr", "bin"), "bash.exe")
        os.vrunv(bash, {
            "-leo",
            "pipefail",
            "-c",
            "pacman --noconfirm -S --needed --overwrite '*' make mingw-w64-x86_64-uchardet " ..
                "mingw-w64-x86_64-gcc mingw-w64-x86_64-iconv mingw-w64-x86_64-pkgconf"
        })
        package:base():script("install")(package)
    end)
    on_test(function (package)
        os.vrun("ffmpeg -version")
    end)
package_end()

option("target_type")
    set_default("client")
    set_showmenu(true)
    set_values("client")
option_end()

add_requires("levilamina 26.10.*", {configs = {target_type = get_config("target_type")}})

add_requires("levibuildscript")

add_requires("stduuid")
add_requires("xxhash")
add_requires("openssl")
add_requires("libzip")
add_requires("imgui v1.92.7", {configs = {dx11 = true, dx12 = true}})
add_requires("playback-ffmpeg 7.1", {
    configs = {
        shared = false,
        gpl = true,
        ffmpeg = true,
        ffprobe = false,
        ffplay = false,
        libx264 = true,
    }
})

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

local get_version = function(os)
    local tag = os.iorun("git describe --tags --abbrev=0 --always")
    local major, minor, patch, suffix = tag:match("v(%d+)%.(%d+)%.(%d+)(.*)")
    if not major then
        print("Failed to parse version tag, using 0.0.0")
        major, minor, patch = 0, 0, 0
    end
    if suffix and suffix ~= "" then
        return major .. "." .. minor .. "." .. patch .. string.gsub(suffix, "%s+$", "")
    end
    return major .. "." .. minor .. "." .. patch
end

target("playback")
    add_rules("@levibuildscript/linkrule")
    add_cxflags( "/EHa", "/utf-8", "/W4", "/w44265", "/w44289", "/w44296", "/w45263", "/w44738", "/w45204")
    add_defines("NOMINMAX", "UNICODE")
    add_packages("levilamina")
    add_packages("stduuid")
    add_packages("xxhash")
    add_packages("openssl")
    add_packages("libzip")
    add_packages("imgui")
    add_syslinks("d3d11", "d3d12", "dxgi", "d3dcompiler", "windowscodecs", "ole32", "comdlg32", "shell32")
    set_exceptions("none") -- To avoid conflicts with /EHa.
    set_kind("shared")
    set_languages("c++20")
    if is_mode("debug") then
        add_defines("DEBUG")
    end
    add_headerfiles("src/**.h")
    add_files("src/**.cpp")
    add_includedirs("src")
    set_symbols("debug")
    on_load(function (target)
        target:add("rules", "@levibuildscript/modpacker", {
            modVersion = get_version(os),
        })
    end)

    after_build(function (target)
        import("utils.archive")
        import("core.project.project")

        local output_dir = path.join(os.projectdir(), "bin", target:name())
        os.mkdir(output_dir)
        os.cp(path.join(os.projectdir(), "LICENSE"), output_dir)
        os.cp(path.join(os.projectdir(), "THIRD_PARTY_NOTICES.md"), output_dir)

        local license_source = path.join(os.projectdir(), "licenses")
        local license_dir = path.join(output_dir, "licenses")
        os.tryrm(license_dir)
        os.cp(license_source, license_dir)

        local lang_source = path.join(os.projectdir(), "src", "lang")
        local lang_dir = path.join(output_dir, "lang")
        os.tryrm(lang_dir)
        os.cp(lang_source, lang_dir)

        local font_source = path.join(os.projectdir(), "assets", "fonts", "lucide.ttf")
        local font_dir    = path.join(output_dir, "fonts")
        assert(os.isfile(font_source), "icon font asset was not found")
        os.tryrm(font_dir)
        os.mkdir(font_dir)
        os.cp(font_source, font_dir)

        local ffmpeg_package = assert(
            project.required_package("playback-ffmpeg"),
            "FFmpeg package was not resolved"
        )
        local ffmpeg_source  = path.join(ffmpeg_package:installdir(), "bin", "ffmpeg.exe")
        local tools_dir      = path.join(output_dir, "tools")
        assert(os.isfile(ffmpeg_source), "bundled ffmpeg executable was not found")
        os.mkdir(tools_dir)
        os.cp(ffmpeg_source, path.join(tools_dir, "ffmpeg.exe"))

        local resource_dir = path.join(os.projectdir(), "resources")
        if os.isdir(resource_dir) then
            local installed_pack = path.join(output_dir, "resource_packs", target:name() .. "-ui")
            local mcpack         = path.join(os.projectdir(), "bin", target:name() .. "-ui.mcpack")
            local mcpack_zip     = mcpack .. ".zip"
            assert(os.isfile(path.join(resource_dir, "manifest.json")), "resource pack manifest.json was not found")
            assert(
                os.isfile(path.join(resource_dir, "ui", "start_screen.json")),
                "main-menu button resource was not found"
            )
            os.tryrm(installed_pack)
            os.cp(resource_dir, installed_pack)
            os.tryrm(mcpack)
            os.tryrm(mcpack_zip)
            archive.archive(mcpack_zip, "*", {
                curdir = resource_dir,
                recurse = true
            })
            os.mv(mcpack_zip, mcpack)
            cprint("${bright green}[Playback]: ${reset}Main-menu button resource pack installed to " .. installed_pack)
            cprint("${bright green}[Playback]: ${reset}Standalone UI resource pack generated to " .. mcpack)
        end
    end)

target("camera-timeline-tests")
    set_default(false)
    set_kind("binary")
    set_languages("c++20")
    add_cxflags("/utf-8")
    add_includedirs("src")
    add_files(
        "tests/camera/CameraTimelineEvaluatorTests.cpp",
        "src/playback/editor/keyframe/CameraTimelineEvaluator.cpp",
        "src/playback/editor/keyframe/CameraTimelineRegistry.cpp",
        "src/playback/editor/editing/commands/CameraCommands.cpp",
        "src/playback/editor/editing/CameraBindingOps.cpp",
        "src/playback/editor/editing/SequenceOps.cpp"
    )
