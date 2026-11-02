# Hardware-independent core: g-force maths, the voxel frame and all effects.
idf_component_register(SRC_DIRS "src" "src/effects"
                       INCLUDE_DIRS "include")

# The ESP32 FPU is single precision only; promotion to double is a silent 10x
# slowdown, so make it an error.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wdouble-promotion -Werror=double-promotion -fno-math-errno)
