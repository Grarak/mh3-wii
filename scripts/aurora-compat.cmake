# FindZLIB/FindPNG can expose the SDK's entire C header directory as -isystem.
# With Homebrew Clang this precedes libc++ wrappers and breaks include_next.
# The SDK headers are already supplied by the compiler's sysroot.
function(mh3_fix_sdk_header_order)
  foreach(sdk_target ZLIB::ZLIB PNG::PNG)
    if(APPLE AND TARGET ${sdk_target})
      get_target_property(sdk_headers ${sdk_target} INTERFACE_INCLUDE_DIRECTORIES)

      if(sdk_headers)
        list(FILTER sdk_headers EXCLUDE REGEX "MacOSX[^/]*\\.sdk/usr/include/?$")
        set_target_properties(${sdk_target} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${sdk_headers}")
      endif()
    endif()
  endforeach()
endfunction()

cmake_language(DEFER CALL mh3_fix_sdk_header_order)

# Include standard declarations before the compatibility SDK headers.
function(mh3_preinclude_standard_headers)
  if(TARGET fmt)
    target_compile_options(fmt PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstdlib -include cfloat>")
  endif()
endfunction()

cmake_language(DEFER CALL mh3_preinclude_standard_headers)
