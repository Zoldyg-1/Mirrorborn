set_xmakever("3.0.0")

set_project("Mirrorborn")
set_version("1.0.0")
set_arch("x64")
set_languages("c++23")
set_encodings("utf-8")
set_warnings("allextra")

add_rules("mode.release", "mode.releasedbg")

includes("lib/commonlibsse-ng")

target("Mirrorborn", function()
    set_kind("shared")
    set_filename("Mirrorborn.dll")
    set_default(true)
    set_runtimes("MD")
    set_optimize("fastest")
    set_symbols("debug")

    add_deps("commonlibsse-ng")
    add_files("src/**.cpp")
    add_headerfiles("include/(CCA/**.h)")
    add_includedirs("include")
    set_pcxxheader("src/pch.h")

    add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
    add_cxxflags(
        "cl::/MP",
        "cl::/permissive-",
        "cl::/Zc:preprocessor",
        "cl::/Zc:__cplusplus",
        "cl::/W4"
    )

    set_targetdir("build/$(mode)/bin")
    set_objectdir("build/$(mode)/obj")

    on_config(function(target)
        if has_config("skyrim_se") or not has_config("skyrim_ae") or has_config("skyrim_vr") then
            raise("Mirrorborn 1.0.0 is intentionally AE 1.6.1170-only. Configure with --skyrim_se=n --skyrim_ae=y --skyrim_vr=n")
        end
    end)
end)
