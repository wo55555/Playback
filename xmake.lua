add_rules("mode.debug", "mode.release")

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

option("target_type")
    set_default("client")
    set_showmenu(true)
    set_values("server", "client")
option_end()

-- add_requires("levilamina x.x.x") for a specific version
-- add_requires("levilamina develop") to use develop version
-- please note that you should add bdslibrary yourself if using dev version
add_requires("levilamina", {configs = {target_type = get_config("target_type")}})

add_requires("levibuildscript")

add_requires("stduuid")
add_requires("xxhash")
add_requires("openssl")
add_requires("libzip")
add_requires("imgui v1.92.7", {configs = {dx12 = true}})

if not has_config("vs_runtime") then
    set_runtimes("MD")
end

target("playback") -- Change this to your mod name.
    add_rules("@levibuildscript/linkrule")
    add_rules("@levibuildscript/modpacker")
    add_cxflags( "/EHa", "/utf-8", "/W4", "/w44265", "/w44289", "/w44296", "/w45263", "/w44738", "/w45204")
    add_defines("NOMINMAX", "UNICODE")
    add_packages("levilamina")
    add_packages("stduuid")
    add_packages("xxhash")
    add_packages("openssl")
    add_packages("libzip")
    add_packages("imgui")
    add_syslinks("d3d12", "dxgi", "d3dcompiler")
    set_exceptions("none") -- To avoid conflicts with /EHa.
    set_kind("shared")
    set_languages("c++20")
    if is_mode("debug") then
        add_defines("DEBUG")
    end
    add_headerfiles("src/**.h")
    add_files("src/**.cpp")
    add_includedirs("src")
    after_build(function(target)
        local license_source = path.join(os.projectdir(), "licenses", "DearImGui-LICENSE.txt")
        local license_dir = path.join(os.projectdir(), "bin", target:name(), "licenses")
        os.mkdir(license_dir)
        os.cp(license_source, path.join(license_dir, "DearImGui-LICENSE.txt"))

        local resource_dir = path.join(os.projectdir(), "resources")
        if os.isdir(resource_dir) then
            local mcpack = path.join(os.projectdir(), "bin", target:name(), target:name() .. "-ui.mcpack")
            local packer = path.join(os.projectdir(), "scripts", "package_resource_pack.ps1")
            os.execv("powershell", {
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                packer,
                resource_dir,
                mcpack
            })
            cprint("${bright green}[Playback]: ${reset}UI resource pack generated to " .. mcpack)
        end
    end)
    -- if is_config("target_type", "server") then
        -- add_includedirs("src-server")
        -- add_files("src-server/**.cpp")
    -- else
        -- add_includedirs("src-client")
        -- add_files("src-client/**.cpp")
    -- end
