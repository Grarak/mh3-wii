# mh3_web_data_blobs(ASSEMBLY OUTPUT): writes OUTPUT, a C++ file that embeds
# the files of the translator's data_sections_init_blobs.S with #embed under
# the same symbols. WebAssembly objects take no ELF section directives.
function(mh3_web_data_blobs assembly output)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${assembly}")
  file(STRINGS "${assembly}" lines)
  set(text "// Generated from ${assembly} (runtime/web/data_blobs.cmake).\n")
  string(APPEND text "#pragma clang diagnostic ignored \"-Wc23-extensions\"\nextern \"C\" {\n")
  set(symbol "")

  foreach(line IN LISTS lines)
    if(line MATCHES "^([A-Za-z_][A-Za-z0-9_]*):$")
      set(symbol "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^\\.incbin \"([^\"]+)\"$")
      if(NOT symbol)
        message(FATAL_ERROR "${assembly}: .incbin without a symbol")
      endif()

      string(APPEND text "extern const unsigned char ${symbol}[];\nalignas(16) const unsigned char ${symbol}[] = {\n#embed \"${CMAKE_MATCH_1}\" if_empty(0)\n};\n")
      set(symbol "")
    endif()
  endforeach()

  string(APPEND text "}\n")

  if(EXISTS "${output}")
    file(READ "${output}" previous)
  endif()

  if(NOT previous STREQUAL text)
    file(WRITE "${output}" "${text}")
  endif()
endfunction()
