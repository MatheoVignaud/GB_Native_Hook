add_rules("mode.debug", "mode.release")

target("GB_Native_Hook")
    set_kind("binary")
    add_files("src/*.c")
    add_includedirs("include")

