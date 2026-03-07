add_rules("mode.debug", "mode.release")

add_requires("libsdl3")

target("GB_Native_Hook")
    set_kind("binary")
    set_languages("c11", "c++20")
    add_files("src/*.c", "src/*.cpp")
    add_includedirs("include", "libs/ViruaPPU/include")
    add_packages("libsdl3")
    add_cflags("-O3")
    if is_host("windows") then
        set_toolchains("mingw")
    end
    if is_plat("windows", "mingw") then
        add_links("ws2_32")
    end


