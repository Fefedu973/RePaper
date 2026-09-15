# Resource-only UI primitives. Compile into the consuming executable so static
# library dead stripping cannot discard the QML resources in tools or UI tests.
function(repaper_use_ui target)
    if(NOT TARGET ${target})
        return()
    endif()
    set(paper_ui_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/qml")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
    file(GLOB paper_ui_files CONFIGURE_DEPENDS "${paper_ui_dir}/*.qml" "${paper_ui_dir}/qmldir")
    qt_add_resources(${target} ${target}_paper_ui PREFIX "/paper" BASE "${paper_ui_dir}" FILES ${paper_ui_files})
endfunction()
