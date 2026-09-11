idf_component_register(
    SRCS "src/guest.c"
    INCLUDE_DIRS "include"
    REQUIRES quickjs-ng
    PRIV_REQUIRES heap log
    REQUIRED_IDF_TARGETS esp32p4 esp32s3
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

# The immutable-buffer fix (reverse/species-create write paths QuickJS-ng
# 0.14.0 misses) used to be applied to managed_components/quickjs-ng at
# build time by tools/prepare_quickjs.py, substituted into the quickjs-ng
# target's SOURCES here. components/quickjs-ng/quickjs-ng/quickjs.c is now
# committed already patched (see components/quickjs-ng's own history), so
# quickjs-ng compiles it as an ordinary vendored source and this component
# has nothing left to rewrite at configure time.
