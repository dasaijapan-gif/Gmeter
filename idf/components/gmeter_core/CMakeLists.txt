# Hardware-independent core: g-force maths, the voxel frame and all effects.
idf_component_register(SRC_DIRS "src" "src/effects"
                       INCLUDE_DIRS "include")

# Smoke grid sized for the ESP32 budget: 16 x 8 cells per layer, ~58 KB of
# heap while the effect is active. PUBLIC so every user of gm_smoke.h agrees.
target_compile_definitions(${COMPONENT_LIB} PUBLIC GM_SMOKE_NX=16 GM_SMOKE_NY=8)
# The ESP32 FPU is single precision only; promotion to double is a silent 10x
# slowdown, so make it an error.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wdouble-promotion -Werror=double-promotion -fno-math-errno)
