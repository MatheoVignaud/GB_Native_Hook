add_rules("mode.debug", "mode.release")

add_requires("libsdl3")

target("GB_Native_Hook")
    set_kind("binary")
    add_files("src/*.c")
    add_includedirs("include")
    add_packages("libsdl3")
    add_links("ws2_32")


