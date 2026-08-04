#pragma once

#if defined(_WIN32)
#  if defined(QFRAMEWORK_BUILD)
#    define QFRAMEWORK_EXPORT __declspec(dllexport)
#  else
#    define QFRAMEWORK_EXPORT __declspec(dllimport)
#  endif
#  if !defined(QFRAMEWORK_PROTOCOLS_EXPORT)
#    if defined(QFRAMEWORK_PROTOCOLS_BUILD)
#      define QFRAMEWORK_PROTOCOLS_EXPORT __declspec(dllexport)
#    else
#      define QFRAMEWORK_PROTOCOLS_EXPORT __declspec(dllimport)
#    endif
#  endif
#else
#  define QFRAMEWORK_EXPORT
#  if !defined(QFRAMEWORK_PROTOCOLS_EXPORT)
#    define QFRAMEWORK_PROTOCOLS_EXPORT
#  endif
#endif
