add_rules("mode.debug", "mode.release")

add_requires("libsdl3", {configs = {toolchains = "mingw"}})

target("GB_Native_Hook")
    set_kind("binary")
    set_languages("c17", "c++20")

    add_files("src/*.c", "src/*.cpp", "libs/ViruaPPU/src/*.c", "libs/VirtuaAPU/src/*.c")
    add_includedirs("include", "libs/ViruaPPU/include", "libs/VirtuaAPU/include")
    add_packages("libsdl3")

    add_defines("USE_OPENMP")

    add_cflags("-fopenmp", {tools = {"gcc", "clang"}})
    add_cxflags("-fopenmp", {tools = {"gcc", "clang"}})
    add_ldflags("-fopenmp", {tools = {"gcc", "clang"}})

    if is_mode("release") then
        add_defines("NDEBUG")

        add_cflags(
            "-Ofast",
            "-flto",
            "-fomit-frame-pointer",
            "-ffunction-sections",
            "-fdata-sections",
            {tools = {"gcc", "clang"}}
        )

        add_cxflags(
            "-Ofast",
            "-flto",
            "-fomit-frame-pointer",
            "-ffunction-sections",
            "-fdata-sections",
            {tools = {"gcc", "clang"}}
        )

        add_ldflags(
            "-flto",
            "-Wl,--gc-sections",
            {tools = {"gcc", "clang"}}
        )
    end

    if is_host("windows") then
        set_toolchains("mingw")
    end

    if is_plat("windows", "mingw") then
        add_links("ws2_32", "gomp")
    end