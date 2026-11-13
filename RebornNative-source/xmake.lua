add_cxflags("/d1trimfile:" .. os.projectdir():match("^[^ ]+"), {force = true})
add_shflags("/PDBALTPATH:RebornNative.pdb", {force = true})

-- include subprojects
includes("lib/commonlibob64")

-- set project constants
set_project("RebornNative")
set_version("0.0.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- define targets
target("RebornNative")
    add_rules("commonlibob64.plugin", {
        name = "RebornNative",
        author = "Oblivion Reborn",
        description = "Native bridge for Oblivion Reborn: inventories and containers"
    })

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
