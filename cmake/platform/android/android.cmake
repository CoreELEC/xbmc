set(PLATFORM_REQUIRED_DEPS LibAndroidJNI OpenGLES EGL LibZip)
set(APP_RENDER_SYSTEM gles)
list(APPEND PLATFORM_OPTIONAL_DEPS LibDovi)

# Store SDK compile version
set(TARGET_SDK 33)
# Minimum supported SDK version
set(TARGET_MINSDK 21)
