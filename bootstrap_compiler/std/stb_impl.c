// Seed-side companion for `--src=file`. The package authority is
// `std_packages/stb_image/native/stb_image_stub.c` plus `vendor/*.h`.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STBI_NO_SIMD

#ifdef __has_include
  #if __has_include("stb_image.h")
    #include "stb_image.h"
  #elif __has_include("../std_packages/stb_image/vendor/stb_image.h")
    #include "../std_packages/stb_image/vendor/stb_image.h"
  #else
    #error "stb_image.h not found"
  #endif

  #if __has_include("stb_image_write.h")
    #include "stb_image_write.h"
  #elif __has_include("../std_packages/stb_image/vendor/stb_image_write.h")
    #include "../std_packages/stb_image/vendor/stb_image_write.h"
  #else
    #error "stb_image_write.h not found"
  #endif

  #if __has_include("stb_image_resize2.h")
    #include "stb_image_resize2.h"
  #elif __has_include("../std_packages/stb_image/vendor/stb_image_resize2.h")
    #include "../std_packages/stb_image/vendor/stb_image_resize2.h"
  #else
    #error "stb_image_resize2.h not found"
  #endif
#else
  #include "stb_image.h"
  #include "stb_image_write.h"
  #include "stb_image_resize2.h"
#endif
