# Fetch CoreCLR sources if necessary
if ("${CORECLR_DIR}" STREQUAL "")
    set(CORECLR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/.coreclr)

    find_package(Git REQUIRED)
    if (EXISTS "${CORECLR_DIR}/.git/config")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} config remote.origin.fetch "+refs/heads/*:refs/remotes/origin/*"
            WORKING_DIRECTORY ${CORECLR_DIR})
        execute_process(
            COMMAND ${GIT_EXECUTABLE} fetch --progress --depth 1 origin "${CORECLR_BRANCH}"
            WORKING_DIRECTORY ${CORECLR_DIR}
            RESULT_VARIABLE retcode)
        if (NOT "${retcode}" STREQUAL "0")
            message(FATAL_ERROR "Fatal error when fetching ${CORECLR_BRANCH} branch")
        endif()
        execute_process(
            COMMAND ${GIT_EXECUTABLE} checkout "${CORECLR_BRANCH}"
            WORKING_DIRECTORY ${CORECLR_DIR}
            RESULT_VARIABLE retcode)
        if (NOT "${retcode}" STREQUAL "0")
            message(FATAL_ERROR "Fatal error when cheking out ${CORECLR_BRANCH} branch")
        endif()
    else()
        if (IS_DIRECTORY "${CORECLR_DIR}")
            file(REMOVE_RECURSE "${CORECLR_DIR}")
        endif()
        execute_process(
            COMMAND ${GIT_EXECUTABLE} clone --progress --depth 1 https://github.com/dotnet/coreclr "${CORECLR_DIR}" -b "${CORECLR_BRANCH}"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            RESULT_VARIABLE retcode)
        if (NOT "${retcode}" STREQUAL "0")
            message(FATAL_ERROR "Fatal error when cloning coreclr sources")
        endif()
    endif()
endif()

# Fetch .NET SDK binaries if necessary
if ("${DOTNET_DIR}" STREQUAL "" AND (("${DBGSHIM_RUNTIME_DIR}" STREQUAL "") OR ${BUILD_MANAGED}))
    set(DOTNET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/.dotnet)

    if (WIN32)
        execute_process(
            COMMAND powershell -Command "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 ; (new-object System.Net.WebClient).DownloadFile('https://dot.net/v1/dotnet-install.ps1', '${CMAKE_CURRENT_BINARY_DIR}/dotnet-install.ps1')"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            RESULT_VARIABLE retcode)
        if (NOT "${retcode}" STREQUAL "0")
            message(FATAL_ERROR "Fatal error when downloading dotnet install script")
        endif()
        execute_process(
            COMMAND powershell -File "${CMAKE_CURRENT_BINARY_DIR}/dotnet-install.ps1" -Channel "${DOTNET_CHANNEL}" -InstallDir "${DOTNET_DIR}" -Architecture x64 -Verbose
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            RESULT_VARIABLE retcode)
        if (NOT "${retcode}" STREQUAL "0")
            message(FATAL_ERROR "Fatal error when installing dotnet")
        endif()
        else()
        execute_process(
            COMMAND bash -c "curl -sSL \"https://dot.net/v1/dotnet-install.sh\" | bash /dev/stdin --channel \"${DOTNET_CHANNEL}\" --install-dir \"${DOTNET_DIR}\" --verbose"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            RESULT_VARIABLE retcode)
        if (NOT "${retcode}" STREQUAL "0")
            message(FATAL_ERROR "Fatal error when installing dotnet")
        endif()
    endif()
endif()
