# PlatformIO build hook: rebuild the embedded library whenever data/library.bin changes.
Import("env")  # noqa: F821  (provided by PlatformIO)

env.Depends("$BUILD_DIR/src/library_blob.S.o", env.File("$PROJECT_DIR/data/library.bin"))  # noqa: F821
