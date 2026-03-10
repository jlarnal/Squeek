"""Pre-build script: delete .o files that embed __DATE__/__TIME__ so they
always recompile with a fresh timestamp."""
import os, glob

Import("env")

build_dir = env.subst("$BUILD_DIR")
# Object files that use __DATE__ / __TIME__
targets = ["debug_cli.cpp.o", "web_server.cpp.o"]

for root, dirs, files in os.walk(build_dir):
    for f in files:
        if f in targets:
            path = os.path.join(root, f)
            try:
                os.remove(path)
            except OSError:
                pass
