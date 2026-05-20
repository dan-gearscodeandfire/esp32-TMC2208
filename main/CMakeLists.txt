idf_component_register(
    SRCS "main.c" "motor.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES "driver" "tmc2208"
)
