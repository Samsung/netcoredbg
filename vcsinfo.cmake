# This file contains functions which helps extract VCS information during build.

# Extract current revision from .git (only .git/HEAD referenced).
# Optional argument: root directory for the source tree.
# Result might be empty string in case of error.
# TODO add same logic for SVN if you need this..
function(VCSInfo result)
    if (${ARGC} GREATER 1)
        set(path "${ARGV1}")
    else()
        get_filename_component(path . REALPATH)
    endif()
    set(git_dir "${path}/.git")

    set(revision "")
    if (IS_DIRECTORY "${git_dir}")
        file(READ "${git_dir}/HEAD" git_head)
        if (git_head MATCHES "^ref:")
            string(REGEX REPLACE "^ref:[ \t]*([^ \t\r\n]+).*" "\\1" git_ref "${git_head}")
            if (EXISTS "${git_dir}/${git_ref}")
                file(READ "${git_dir}/${git_ref}" revision)
                string(REGEX MATCH "^......." revision "${revision}")
            else()
                set(revision ${git_ref})
            endif()
        else()
            set(revision ${git_head})
            string(REGEX MATCH "^......." revision "${revision}")
        endif()

    elseif((NOT path STREQUAL "/") AND (NOT ${path} MATCHES ".*:/$"))
        get_filename_component(parent_dir "${path}" PATH)
        VCSInfo(revision ${parent_dir})
    endif()

    if (revision STREQUAL "")
        set(${result} "\"not detected\"" PARENT_SCOPE)
    else()
        set(${result} "${revision}" PARENT_SCOPE)
    endif()
endfunction(VCSInfo)
