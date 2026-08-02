add_rules("mode.debug", "mode.release")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

option("target_type")
    set_default("client")
    set_showmenu(true)
    set_values("client")
option_end()

add_requires("levilamina 26.10.*", {configs = {target_type = get_config("target_type")}})

add_requires("levibuildscript")

-- 内置静态 FFmpeg（GPL）：先运行 scripts/build-ffmpeg/build-ffmpeg.ps1 生成 third_party/ffmpeg 产物
option("playback_ffmpeg")
    set_showmenu(true)
    set_default(os.isdir("third_party/ffmpeg/include") and os.isdir("third_party/ffmpeg/lib"))
    set_description("Link built-in static FFmpeg from third_party/ffmpeg (GPL)")
option_end()

local function setup_ffmpeg(target)
    if has_config("playback_ffmpeg") then
        target:add("defines", "PLAYBACK_HAVE_FFMPEG=1")
        target:add("includedirs", "third_party/ffmpeg/include")
        target:add("linkdirs", "third_party/ffmpeg/lib")
        target:add("links", "avformat", "avcodec", "avutil", "swscale", "swresample",
                   "x264", "x265", "vpx", "opus", "zlib")
        target:add("syslinks", "bcrypt", "ws2_32", "secur32", "avrt", "user32", "ole32")
    end
end

add_requires("stduuid")
add_requires("xxhash")
add_requires("openssl")
add_requires("libzip")
add_requires("imgui v1.92.7", {configs = {dx11 = true, dx12 = true}})

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
    setup_ffmpeg(target)
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

target("refactor-model-tests")
    set_default(false)
    set_kind("binary")
    set_languages("c++20")
    add_includedirs("src")
    add_files("tests/refactor/models/ModelTests.cpp")
    add_files("src/playback/refactor/editor/models/SelectionModel.cpp")
    add_files("src/playback/refactor/editor/CommandStack.cpp")
    add_files("src/playback/refactor/editor/CommandFactory.cpp")
    add_files("src/playback/refactor/video-editing/SequenceOps.cpp")
    add_files("src/playback/refactor/video-editing/WorldActorOps.cpp")
    add_files("src/playback/refactor/video-editing/CameraBindingOps.cpp")
    add_files("src/playback/refactor/video-editing/commands/SequenceCommands.cpp")
    add_files("src/playback/refactor/video-editing/commands/WorldActorCommands.cpp")
    add_files("src/playback/refactor/video-editing/commands/CameraCommands.cpp")
    add_files("src/playback/refactor/video-editing/commands/SubActorCommands.cpp")
    add_files("src/playback/refactor/video-editing/EditingCommands.cpp")
    after_build(function (target)
        import("utils.archive")

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

        local resource_dir = path.join(os.projectdir(), "resources")
        if os.isdir(resource_dir) then
            local installed_pack = path.join(output_dir, "resource_packs", target:name() .. "-ui")
            local mcpack = path.join(os.projectdir(), "bin", target:name() .. "-ui.mcpack")
            local mcpack_zip = mcpack .. ".zip"
            assert(os.isfile(path.join(resource_dir, "manifest.json")), "resource pack manifest.json was not found")
            os.tryrm(installed_pack)
            os.cp(resource_dir, installed_pack)
            os.tryrm(mcpack)
            os.tryrm(mcpack_zip)
            archive.archive(mcpack_zip, "*", {
                curdir = resource_dir,
                recurse = true
            })
            os.mv(mcpack_zip, mcpack)
            cprint("${bright green}[Playback]: ${reset}UI resource pack installed to " .. installed_pack)
            cprint("${bright green}[Playback]: ${reset}Standalone UI resource pack generated to " .. mcpack)
        end
    end)

-- FFmpeg 链接 smoke test（步骤 1 验证：静态库可链接、libav 可初始化）
if has_config("playback_ffmpeg") then
    target("refactor-ffmpeg-tests")
        set_default(false)
        set_kind("binary")
        set_languages("c++20")
        setup_ffmpeg(target)
        add_includedirs("src")
        add_files("tests/refactor/ffmpeg/FfmpegSmokeTest.cpp")
        add_files("src/playback/refactor/export-pipeline/FfmpegBuildInfo.cpp")
end
