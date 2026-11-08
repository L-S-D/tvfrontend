stb_image.h (v2.30) and stb_image_write.h (v1.16) from https://github.com/nothings/stb,
commit 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5, unchanged (public domain / MIT).
Used by src/ui/imgtrim.c to trim channel logos in the fetch threads (LVGL's
lodepng decodes into LVGL draw buffers and must stay on the main thread).
