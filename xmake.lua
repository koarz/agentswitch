set_project("agentswitch")
set_version("0.1.0")
set_xmakever("2.8.0")

set_languages("c++20")
set_warnings("all")
set_toolchains("clang")
add_rules("mode.debug", "mode.release")

-- DuckDB's official prebuilt shared library (C API). Building the
-- amalgamation from source takes several minutes, this is a download.
package("duckdb-bin")
    set_homepage("https://duckdb.org")
    set_description("DuckDB prebuilt shared library and C header")
    add_urls("https://github.com/duckdb/duckdb/releases/download/v$(version)/libduckdb-linux-amd64.zip")
    add_versions("1.5.6", "b845005f5132a7d8180057c35e14a7626632258782f871a90861b19c1c03841b")

    on_install("linux|x86_64", function (package)
        os.cp("duckdb.h", package:installdir("include"))
        os.cp("libduckdb.so", package:installdir("lib"))
    end)

    on_test(function (package)
        assert(package:has_cfuncs("duckdb_open", {includes = "duckdb.h"}))
    end)
package_end()

add_requires("duckdb-bin 1.5.6")
add_requires("ftxui v5.0.0")
add_requires("nlohmann_json")

target("agentswitch")
    set_kind("binary")
    add_files("src/*.cpp")
    add_packages("duckdb-bin", "ftxui", "nlohmann_json")
    add_syslinks("curl", "pthread")

    -- libduckdb.so is found next to the executable (build dir) or in ../lib
    -- (installed layout: <prefix>/bin/agentswitch, <prefix>/lib/libduckdb.so)
    add_ldflags("-Wl,-rpath,$ORIGIN:$ORIGIN/../lib", {force = true})
    after_build(function (target)
        local pkg = target:pkg("duckdb-bin")
        if pkg then
            for _, libdir in ipairs(pkg:get("linkdirs") or {}) do
                local so = path.join(libdir, "libduckdb.so")
                if os.isfile(so) then
                    os.cp(so, target:targetdir())
                    break
                end
            end
        end
    end)

    after_install(function (target)
        local so = path.join(target:targetdir(), "libduckdb.so")
        if os.isfile(so) then
            os.mkdir(path.join(target:installdir(), "lib"))
            os.cp(so, path.join(target:installdir(), "lib"))
        end
    end)
