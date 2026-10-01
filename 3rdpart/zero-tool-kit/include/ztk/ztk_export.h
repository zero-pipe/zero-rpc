#ifndef ZTK_EXPORT_H
#define ZTK_EXPORT_H

#if defined(ZTK_BUILD_SHARED)
#  if defined(_WIN32) && defined(ZTK_EXPORTS)
#    define ZTK_API __declspec(dllexport)
#  elif defined(_WIN32)
#    define ZTK_API __declspec(dllimport)
#  elif defined(__GNUC__)
#    define ZTK_API __attribute__((visibility("default")))
#  else
#    define ZTK_API
#  endif
#else
#  define ZTK_API
#endif

#endif /* ZTK_EXPORT_H */
