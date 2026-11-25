# Validates the EPUB test books with EPUBCheck (https://www.w3.org/publishing/epubcheck/), the validator that
# eBook stores use. Run by CTest as:
#   cmake -DEPUB_TEST=<epub_test> -DJAVA=<java> -DEPUBCHECK_JAR=<epubcheck.jar> -DOUT=<folder> -P epubcheck.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/books")
set(ENV{NOVELCLANKER_EPUB_OUT} "${OUT}/books")
execute_process(COMMAND "${EPUB_TEST}" "${OUT}/data" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "epub_test failed")
endif()

file(GLOB books "${OUT}/books/*.epub")
if(NOT books)
    message(FATAL_ERROR "epub_test wrote no books to check")
endif()
foreach(book IN LISTS books)
    execute_process(COMMAND "${JAVA}" -jar "${EPUBCHECK_JAR}" "${book}"
                    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
    get_filename_component(name "${book}" NAME)
    # Warnings count too: a book should pass without any.
    if(NOT result EQUAL 0 OR NOT output MATCHES "No errors or warnings detected")
        message(FATAL_ERROR "${name}:\n${output}")
    endif()
    message(STATUS "${name}: valid")
endforeach()
