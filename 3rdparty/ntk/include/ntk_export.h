#ifndef NTK_EXPORT_H
#define NTK_EXPORT_H

#if defined(NTK_BUILD_SHARED)
#  if defined(_WIN32) && defined(NTK_EXPORTS)
#    define NTK_API __declspec(dllexport)
#  elif defined(_WIN32)
#    define NTK_API __declspec(dllimport)
#  elif defined(__GNUC__)
#    define NTK_API __attribute__((visibility("default")))
#  else
#    define NTK_API
#  endif
#else
#  define NTK_API
#endif

#endif /* NTK_EXPORT_H */
